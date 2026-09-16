// PortAudio mono float32 audio input - implementation. See audio_in.hpp for
// the threading model and for why it lives beside audio_out.cpp.
//
// ANDROID, AND WHY THIS FILE IS GUARDED RATHER THAN TWINNED. The output side
// has two whole files (audio_out.cpp for PortAudio, audio_out_aaudio.cpp for
// AAudio) because an AAudio OUTPUT stream is a real second implementation,
// with its own priming, ring and underrun accounting. There is no second
// implementation here yet: Android has no microphone path in this slice at
// all - AAudio input needs the RECORD_AUDIO runtime permission, which needs a
// Java permission request, which is a slice of its own. So what differs is
// exactly the six entry points that name PortAudio, and they differ by
// REFUSING: an empty device list and a failed open, which is the same pair a
// desktop with no input device produces and which the TRANSMIT page already
// knows how to show.
//
// The four members that are NOT guarded are the ones with no platform in them
// - read, drain, takePeak and the realtime pushBlock core - and they stay
// shared on purpose: they are the half a test can exercise (there is no
// microphone on the bench either), and an Android twin holding a second copy
// of the peak arithmetic would be a second place for it to drift.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/audio_in.hpp"

#if defined(CASCADE_ANDROID)
#include "core/diag_log.hpp"
#else
#include <portaudio.h>
#endif

#include <cmath>

namespace cascade::sink {

// The callback path may not lock. If either of these atomics fell back to a
// lock-based implementation the peak store / overrun bump inside pushBlock
// would block the realtime audio thread, so fail the build instead.
static_assert(std::atomic<float>::is_always_lock_free,
              "the peak atomic must be lock-free for the audio callback");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the overrun counter must be lock-free for the audio callback");

#if !defined(CASCADE_ANDROID)

namespace {

// C trampoline handed to Pa_OpenStream. Deliberately does nothing beyond
// pushBlock, so every behaviour the realtime thread executes is reachable -
// and therefore testable - through the public static pushBlock().
int paInCallback(const void* input, void* /*output*/, unsigned long frameCount,
                 const PaStreamCallbackTimeInfo* /*timeInfo*/,
                 PaStreamCallbackFlags /*statusFlags*/, void* userData) {
    AudioIn::pushBlock(userData, static_cast<const float*>(input),
                       static_cast<std::size_t>(frameCount));
    return paContinue;  // the stream runs until close(); a full ring drops
}

}  // namespace

#endif  // !CASCADE_ANDROID

#if defined(CASCADE_ANDROID)

// -- ANDROID: NO MICROPHONE YET, SAID OUT LOUD ----------------------------
//
// paOk_ stays false, which is the flag every other method already branches
// on: an object that failed to initialise its audio library. The desktop has
// always had that state (PortAudio refusing to start) and every caller
// already copes with it, so nothing above this layer needed a new branch.

AudioIn::AudioIn() : ring_(kRingCapacity) {}

AudioIn::~AudioIn() = default;

std::vector<AudioDevice> AudioIn::listInputDevices() { return {}; }

bool AudioIn::open(int deviceIndex, double sampleRateHz) {
    (void)deviceIndex;
    (void)sampleRateHz;
    // Once per attempt, and in the log rather than only as a false return:
    // the TRANSMIT page shows "no input device", which is true but does not
    // say WHY, and the diagnostics ring is where the why belongs.
    cascade::core::diagWarnf(
        "audio in: Android has no microphone path yet (AAudio input needs the "
        "RECORD_AUDIO runtime permission) - transmit audio is unavailable");
    return false;
}

void AudioIn::close() {}

bool AudioIn::streamAlive() const { return false; }

#else

AudioIn::AudioIn() : ring_(kRingCapacity) { paOk_ = (Pa_Initialize() == paNoError); }

AudioIn::~AudioIn() {
    close();
    // Guarded release: Pa_Terminate() must pair with a SUCCESSFUL
    // Pa_Initialize() - PortAudio refcounts the pairs across instances.
    if (paOk_) { Pa_Terminate(); }
}

std::vector<AudioDevice> AudioIn::listInputDevices() {
    std::vector<AudioDevice> devices;
    if (!paOk_) { return devices; }
    const PaDeviceIndex count = Pa_GetDeviceCount();
    const PaDeviceIndex def = Pa_GetDefaultInputDevice();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        // Output-only devices are not microphones; skip them rather than
        // offering entries that cannot be opened.
        if (info == nullptr || info->maxInputChannels < 1) { continue; }
        devices.push_back(AudioDevice{static_cast<int>(i),
                                      info->name != nullptr ? std::string(info->name)
                                                            : std::string(),
                                      i == def});
    }
    return devices;
}

bool AudioIn::open(int deviceIndex, double sampleRateHz) {
    if (!paOk_ || !(sampleRateHz > 0.0)) { return false; }
    close();  // re-open semantics: switching device goes through here

    const PaDeviceIndex dev = (deviceIndex < 0) ? Pa_GetDefaultInputDevice()
                                                : static_cast<PaDeviceIndex>(deviceIndex);
    if (dev == paNoDevice || dev < 0 || dev >= Pa_GetDeviceCount()) { return false; }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
    if (info == nullptr || info->maxInputChannels < 1) { return false; }

    // Drop anything left from a previous session, so a reopen starts from
    // silence rather than from something said before the device changed.
    // Safe single-threaded consumption: no callback exists yet.
    drain();

    PaStreamParameters in{};
    in.device = dev;
    in.channelCount = 1;
    in.sampleFormat = paFloat32;
    // Low-latency hint: the delay between speaking and the carrier being
    // modulated is this plus the transmit buffer, and both are audible to
    // anybody working duplex.
    in.suggestedLatency = info->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    if (Pa_OpenStream(&stream, &in, nullptr, sampleRateHz, paFramesPerBufferUnspecified,
                      paNoFlag, &paInCallback, this) != paNoError) {
        return false;
    }
    if (Pa_StartStream(stream) != paNoError) {
        Pa_CloseStream(stream);
        return false;
    }
    stream_ = stream;
    running_ = true;
    // Remember what this open WAS, so a later recovery can reproduce it: the
    // request unresolved (-1 stays -1) and the name of what was actually
    // opened. recoveryDeviceIndex() in audio_out.hpp takes exactly this pair
    // and is used unchanged for the microphone.
    everOpened_ = true;
    openedRequested_ = deviceIndex;
    openedName_ = info->name != nullptr ? std::string(info->name) : std::string();
    return true;
}

void AudioIn::close() {
    if (stream_ == nullptr) { return; }
    // Abort rather than Stop: Stop would block until the device drained, and
    // there is nothing in an input queue worth waiting for.
    Pa_AbortStream(static_cast<PaStream*>(stream_));
    Pa_CloseStream(static_cast<PaStream*>(stream_));
    stream_ = nullptr;
    running_ = false;
}

bool AudioIn::streamAlive() const {
    if (stream_ == nullptr) { return false; }
    // 1 = the callback is being called; 0 = stopped; negative = the host API
    // cannot answer, which is what a vanished device tends to produce. Only 1
    // means a microphone is actually being heard.
    return Pa_IsStreamActive(static_cast<PaStream*>(stream_)) == 1;
}

#endif  // CASCADE_ANDROID

// -- PLATFORM-FREE FROM HERE ON --------------------------------------------
//
// Everything below is the ring, the meter and the realtime callback core, and
// none of it names an audio library. Shared by both branches above.

std::size_t AudioIn::read(float* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    return ring_.read(dst, n);
}

void AudioIn::drain() {
    float scratch[256];
    while (ring_.read(scratch, sizeof(scratch) / sizeof(scratch[0])) != 0) {}
}

float AudioIn::takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }

std::size_t AudioIn::pushBlock(void* self, const float* src, std::size_t frames) {
    auto* ai = static_cast<AudioIn*>(self);
    if (ai == nullptr || frames == 0) { return 0; }
    if (src == nullptr) {
        // PortAudio is allowed to hand a null input buffer (a device that
        // produced nothing this period). Not an overrun and not an error:
        // there is simply nothing to push.
        return 0;
    }

    // The peak first, because it must reflect what the microphone HEARD
    // rather than what the ring took: a meter that went quiet whenever the
    // ring was full would say the room had gone silent at exactly the moment
    // the transmitter was in trouble.
    float peak = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        const float a = std::fabs(src[i]);
        // NaN fails every ordering test, so it can never raise the peak -
        // which is the wanted behaviour: a meter must not be pinned by a
        // number that is not one.
        if (a > peak) { peak = a; }
    }
    float seen = ai->peak_.load(std::memory_order_relaxed);
    while (peak > seen &&
           !ai->peak_.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) {
        // seen has been reloaded by the exchange; loop again only while this
        // block is still the louder of the two.
    }

    const std::size_t took = ai->ring_.write(src, frames);
    if (took < frames) {
        // DROPPED, not blocked. See the file header: back-pressuring a
        // realtime audio callback takes the whole input stream down, and one
        // overrun is a gap while a stopped device is silence for ever.
        ai->overruns_.fetch_add(1, std::memory_order_relaxed);
    }
    return took;
}

}  // namespace cascade::sink
