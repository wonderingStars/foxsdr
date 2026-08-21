// PortAudio mono float32 audio output sink — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/audio_out.hpp"

#include <portaudio.h>

#include <cstring>

namespace cascade::sink {

// The callback path may not lock. If either of these atomics fell back to a
// lock-based implementation the volume load / underrun bump inside pullBlock
// would block the realtime audio thread, so fail the build instead.
static_assert(std::atomic<float>::is_always_lock_free,
              "volume atomic must be lock-free for the audio callback");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "underrun counter must be lock-free for the audio callback");

namespace {

// C trampoline handed to Pa_OpenStream. Deliberately does nothing beyond
// pullBlock so every behavior the realtime thread executes is reachable —
// and therefore testable — through the public static pullBlock().
int paOutCallback(const void* /*input*/, void* output, unsigned long frameCount,
                  const PaStreamCallbackTimeInfo* /*timeInfo*/,
                  PaStreamCallbackFlags /*statusFlags*/, void* userData) {
    AudioOut::pullBlock(userData, static_cast<float*>(output),
                        static_cast<std::size_t>(frameCount));
    return paContinue;  // stream runs until close(); starvation plays silence
}

}  // namespace

AudioOut::AudioOut() : ring_(kRingCapacity) {
    paOk_ = (Pa_Initialize() == paNoError);
}

AudioOut::~AudioOut() {
    close();
    // Guarded release: Pa_Terminate() must pair with a SUCCESSFUL
    // Pa_Initialize() — PortAudio refcounts the pairs across instances.
    if (paOk_) { Pa_Terminate(); }
}

std::vector<AudioDevice> AudioOut::listOutputDevices() {
    std::vector<AudioDevice> devices;
    if (!paOk_) { return devices; }
    const PaDeviceIndex count = Pa_GetDeviceCount();
    const PaDeviceIndex def = Pa_GetDefaultOutputDevice();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        // Input-only devices (microphones) are not output sinks; skip them
        // rather than presenting un-openable entries in the Sinks panel.
        if (info == nullptr || info->maxOutputChannels < 1) { continue; }
        devices.push_back(AudioDevice{
            static_cast<int>(i),
            info->name != nullptr ? std::string(info->name) : std::string(),
            i == def});
    }
    return devices;
}

bool AudioOut::open(int deviceIndex, double sampleRateHz, int channels) {
    if (!paOk_ || !(sampleRateHz > 0.0)) { return false; }
    if (channels != 1 && channels != 2) { return false; }
    // Re-open semantics: switching device or rate through open() closes the
    // old stream first. close() is idempotent, so this is safe when nothing
    // is open yet.
    close();

    const PaDeviceIndex dev = (deviceIndex < 0)
                                  ? Pa_GetDefaultOutputDevice()
                                  : static_cast<PaDeviceIndex>(deviceIndex);
    if (dev == paNoDevice || dev < 0 || dev >= Pa_GetDeviceCount()) {
        return false;
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
    if (info == nullptr || info->maxOutputChannels < channels) { return false; }

    // Drain anything left from a previous session so a reopen starts silent
    // instead of replaying stale audio. Safe single-threaded consumption:
    // no callback exists yet, so this thread is the only reader. Bounded by
    // the ring's fill level, which cannot grow here (no producer is active
    // during open()).
    float scratch[256];
    while (ring_.read(scratch, sizeof(scratch) / sizeof(scratch[0])) != 0) {}

    PaStreamParameters out{};
    out.device = dev;
    // 1 = the mono demod chain; 2 = interleaved L,R from the stereo FM path.
    out.channelCount = channels;
    out.sampleFormat = paFloat32;
    // Low-latency hint: an SDR should respond to tuning without an audible
    // lag. PortAudio rounds this up to whatever the host API can honor.
    out.suggestedLatency = info->defaultLowOutputLatency;
    out.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    // paNoFlag keeps PortAudio's default out-of-range clipping enabled: a
    // demod transient beyond ±1.0 gets clamped instead of wrapping into
    // full-scale noise on some host APIs.
    if (Pa_OpenStream(&stream, nullptr, &out, sampleRateHz,
                      paFramesPerBufferUnspecified, paNoFlag, &paOutCallback,
                      this) != paNoError) {
        return false;
    }
    if (Pa_StartStream(stream) != paNoError) {
        Pa_CloseStream(stream);
        return false;
    }
    stream_ = stream;
    running_ = true;
    // Remember what this open WAS, so a later recovery can reproduce it. The
    // request is stored unresolved (-1 stays -1) and the name comes from the
    // device actually opened; together they survive the device list being
    // renumbered underneath us. See recoveryDeviceIndex().
    everOpened_ = true;
    openedRequested_ = deviceIndex;
    openedName_ = info->name != nullptr ? std::string(info->name) : std::string();
    // Only a SUCCESSFUL open changes the layout: a refused stereo open must
    // leave the previous (working) layout description in place, because the
    // caller's fallback path re-opens mono and the producer reads channels()
    // to decide what to push.
    channels_ = channels;
    return true;
}

void AudioOut::close() {
    if (stream_ == nullptr) { return; }  // idempotent / close-without-open
    // Abort rather than Stop: Stop would block until the device drains its
    // queued buffers (tens of ms of already-heard audio); the stop button
    // should cut output immediately. Any samples still in the ring are
    // discarded by the next open()'s drain.
    Pa_AbortStream(static_cast<PaStream*>(stream_));
    Pa_CloseStream(static_cast<PaStream*>(stream_));
    stream_ = nullptr;
    running_ = false;
}

bool AudioOut::streamAlive() const {
    if (stream_ == nullptr) { return false; }
    // Pa_IsStreamActive: 1 = the callback is being called, 0 = stopped, and a
    // negative PaError when the host API cannot answer at all — which is what
    // a vanished device tends to produce. Only 1 means the user is hearing
    // anything, so every other answer is treated as dead. Cheap enough to
    // call at a polling rate: it reads stream state, it does not touch the
    // device.
    return Pa_IsStreamActive(static_cast<PaStream*>(stream_)) == 1;
}

int recoveryDeviceIndex(int lastRequested, const std::string& lastName,
                        const std::vector<AudioDevice>& present) {
    if (lastRequested < 0) { return -1; }  // "follow the default" is the intent
    // Exact pairing first. PortAudio lists one physical device once per host
    // API, so names are NOT unique — matching on the name alone would answer
    // with whichever duplicate came first and quietly move the stream to a
    // different host API than the one that was open. If the remembered index
    // still carries the remembered name, nothing moved: reopen it as it was.
    for (const auto& d : present) {
        if (d.index == lastRequested && d.name == lastName) { return d.index; }
    }
    // The pairing is gone, so the list renumbered (or the device did move
    // host APIs). Now the name is the best handle there is.
    for (const auto& d : present) {
        if (d.name == lastName) { return d.index; }
    }
    return -1;  // the chosen device is gone; the default is the only fallback
}

int clampDeviceRow(int row, const std::vector<AudioDevice>& present) {
    const int n = static_cast<int>(present.size());
    if (n == 0) {
        return -1;  // the combo's "No audio output devices" branch
    }
    if (row >= 0 && row < n) {
        return row;  // still valid: a selection that works is never moved
    }
    // Out of range: the list shrank under a remembered row (or nothing has
    // been chosen yet). Prefer the default device's row; LAST match wins, as
    // the panel's own default-seeking loop has always done.
    int fallback = 0;
    for (int i = 0; i < n; ++i) {
        if (present[static_cast<std::size_t>(i)].isDefault) {
            fallback = i;
        }
    }
    return fallback;
}

std::size_t AudioOut::write(const float* samples, std::size_t n) {
    // SpscRing::write already caps at free space and never blocks; the
    // accepted count is the whole contract.
    return ring_.write(samples, n);
}

std::size_t AudioOut::writeStereo(const float* interleaved, std::size_t frames) {
    // Whole frames only. freeSpace() is conservative for the producer (it can
    // under-report while the callback is mid-read, never over-report), so
    // capping here cannot make ring_.write split the last frame — which would
    // swap L and R for the rest of the session.
    const std::size_t roomFrames = ring_.freeSpace() / 2;
    if (frames > roomFrames) { frames = roomFrames; }
    if (frames == 0) { return 0; }
    return ring_.write(interleaved, frames * 2) / 2;
}

void AudioOut::setVolume(float v01) {
    // Negated comparison so NaN (which fails every ordering test) lands on
    // the silent side instead of poisoning every output sample.
    if (!(v01 >= 0.0f)) { v01 = 0.0f; }
    if (v01 > 1.0f) { v01 = 1.0f; }
    volume_.store(v01, std::memory_order_relaxed);
}

std::size_t AudioOut::pullBlock(void* self, float* dst, std::size_t frames) {
    auto* ao = static_cast<AudioOut*>(self);
    // The device buffer holds frames * channels interleaved floats; the ring
    // already stores them in that exact order, so this stays one flat read.
    const std::size_t n =
        frames * static_cast<std::size_t>(ao->channels_ == 2 ? 2 : 1);
    // Read straight into the device buffer, then scale in place — one pass,
    // no intermediate storage, nothing the realtime thread must wait on. The
    // same `vol` multiplies every sample, so both channels are scaled
    // identically by construction.
    const std::size_t got = ao->ring_.read(dst, n);
    const float vol = ao->volume_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < got; ++i) { dst[i] *= vol; }
    if (got < n) {
        // Starved: the device buffer beyond `got` is whatever the host left
        // there (often the previous period — an audible stutter-loop).
        // Silence it and count ONE underrun event for this callback; the
        // event count is what buffer-health UI wants, not a sample count.
        // A got == n == 0 call never reaches here: a zero-length request
        // cannot starve.
        std::memset(dst + got, 0, (n - got) * sizeof(float));
        ao->underruns_.fetch_add(1, std::memory_order_relaxed);
    }
    return got;
}

}  // namespace cascade::sink
