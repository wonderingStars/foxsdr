// PortAudio float32 mono audio INPUT - the microphone, for the transmitter.
//
// WHY IT IS IN src/sink NEXT TO audio_out.hpp AND NOT IN A src/audio OF ITS
// OWN. Everything this file does is the mirror image of audio_out.cpp, down
// to the device list, the recovery-by-name rule and the shape of the
// callback, and the two want reading together - a change to how a device is
// chosen has to be made in both or it is a bug in one. The directory name is
// wrong for a microphone and the alternative was worse: a third place
// PortAudio is spoken to, globbed by a CMake line that would have to be added
// for one file. (CMakeLists.txt globs src/<dir>/*.cpp per directory, so a new
// directory is a build-system change, not a filesystem one.)
//
// Threading model, and it is audio_out's with the arrows reversed:
//
//   PortAudio callback thread          TX thread
//   ─────────────────────────          ─────────
//   pushBlock(this, in, frames) ─SPSC─▶ read(samples, n)
//                                ring
//
// The callback is the PRODUCER here and it may not block, allocate, lock or
// do I/O: it runs on a realtime audio thread where any of those is a dropout.
// So it does one ring write and one counter bump, and when the ring is full
// it DROPS - a microphone whose samples nobody is taking is a microphone
// nobody is transmitting, and back-pressuring the audio device to say so
// would take the whole input stream down.
//
// pushBlock() is public and static (taking the object through void*) for the
// same reason AudioOut::pullBlock is: it is the exact code the realtime
// thread runs, and a test must be able to run it - with a scripted block of
// audio, on one thread, with no microphone anywhere near the machine. There
// is no microphone on the bench this was written on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "sink/audio_out.hpp"

namespace cascade::sink {

class AudioIn {
public:
    // Calls Pa_Initialize(). PortAudio refcounts paired Initialize/Terminate
    // across instances, so an AudioIn and an AudioOut living together is
    // fine; the guard here is remembering whether OUR Initialize succeeded so
    // the destructor never calls Pa_Terminate() unmatched.
    AudioIn();
    ~AudioIn();

    AudioIn(const AudioIn&) = delete;
    AudioIn& operator=(const AudioIn&) = delete;

    // Every device with at least one INPUT channel, in PortAudio index order.
    // Empty if PortAudio failed to initialize. Same AudioDevice row the Sinks
    // panel already knows how to draw, and `isDefault` marks the host API's
    // default INPUT device rather than its output.
    std::vector<AudioDevice> listInputDevices();

    // Opens deviceIndex (-1 = system default input) as a float32 mono
    // callback stream at sampleRateHz and starts it. An already-open stream
    // is closed first, so open() doubles as "switch device". Returns false on
    // any PortAudio failure, leaving the object closed.
    //
    // MONO ONLY, deliberately. A transmitter modulates one signal; a stereo
    // microphone would have to be downmixed somewhere, and doing it here
    // would mean this file having an opinion about which channel a headset's
    // boom is on. PortAudio opens the device's first input channel.
    bool open(int deviceIndex, double sampleRateHz);

    void close();  // idempotent

    bool running() const { return running_; }

    // True while a stream is open AND PortAudio still reports it running.
    // Exactly AudioOut::streamAlive()'s question and for exactly its reason:
    // a USB headset that re-enumerates takes its stream with it and PortAudio
    // has no callback to say so - the callback simply stops being called, and
    // a transmitter whose microphone died is one putting a dead carrier out.
    bool streamAlive() const;

    bool everOpened() const { return everOpened_; }
    int openedDeviceRequested() const { return openedRequested_; }
    const std::string& openedDeviceName() const { return openedName_; }

    // Consumer side, called from the TX thread. Returns how many samples were
    // available (< n when the microphone has not produced them yet - the
    // caller pads or waits; it is never made to wait for audio here). The one
    // thing it can wait for is a drain() in progress on another thread, which
    // is a bounded in-memory copy of at most one ring (consumerMutex_).
    std::size_t read(float* dst, std::size_t n);

    // How many samples are waiting. The TX thread uses it to decide whether a
    // whole block is ready before it modulates one.
    std::size_t available() const { return ring_.size(); }

    // Drops everything waiting. Called when the transmitter keys up, because
    // what is in the ring at that moment is whatever was said BEFORE the PTT
    // was pressed - the microphone runs whether or not anyone is
    // transmitting, and putting the seconds before the key-down on the air is
    // the one thing this feature must never do.
    void drain();

    // Cumulative count of callbacks whose samples did not all fit (one per
    // callback, not per sample): the TX thread is not keeping up, or nobody
    // is reading at all. Monotonic over the object's lifetime.
    std::uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

    // The loudest sample seen since the last call, in [0, 1], and the reading
    // is CLEARED by asking - so the panel's meter shows the peak of the
    // interval it is drawing rather than the peak since the device opened.
    // Read from the GUI thread; written by the realtime callback, which is
    // why it is one atomic exchange and not a buffer.
    float takePeak();

    // The callback core: `frames` mono float samples pushed into the ring.
    // Static + void* so it is callable both from the C callback and from a
    // test with a scripted block; it must stay lock-free and allocation-free.
    // Returns how many samples the ring accepted.
    static std::size_t pushBlock(void* self, const float* src, std::size_t frames);

private:
    // 32768 samples - 682 ms at 48 kHz. Far deeper than the transmitter needs
    // (it takes a block every few milliseconds), and deliberately so: the
    // cost of depth here is latency the drain() above removes at every
    // key-down, and the benefit is that a GUI-thread hiccup does not punch a
    // hole in somebody's sentence. Power of two as SpscRing requires.
    static constexpr std::size_t kRingCapacity = std::size_t{1} << 15;

    dsp::SpscRing<float> ring_;
    // THE RING HAS ONE PRODUCER AND TWO WOULD-BE CONSUMERS: the TX thread's
    // read(), and drain() - called by the Transmitter (under its own state
    // lock) and by open(), which since 2026-09-24 runs on a gui::AudioOpen
    // worker while the frame loop, and therefore the key, stays live. An SPSC
    // ring read from two threads at once can move its read index past the
    // write index, so the two are serialised here. The realtime PRODUCER
    // (pushBlock) never takes it.
    std::mutex consumerMutex_;
    std::atomic<std::uint64_t> overruns_{0};
    // Written by the realtime callback, exchanged to 0 by takePeak().
    std::atomic<float> peak_{0.0f};
    void* stream_ = nullptr;  // PaStream*, as audio_out.hpp does it
    bool paOk_ = false;
    bool running_ = false;
    bool everOpened_ = false;
    int openedRequested_ = -1;
    std::string openedName_;
};

}  // namespace cascade::sink
