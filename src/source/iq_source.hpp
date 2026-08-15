// The source abstraction every IQ producer implements: the signal generator,
// IQ file playback, and real hardware through SoapySDR.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>

namespace cascade::source {

// Pull-based IQ sample source, consumed by the Pipeline's source thread.
//
// Pacing contract: selfPaced() distinguishes the two producer families.
//  - Self-paced sources (hardware) deliver samples at the rate the device
//    produces them: read() blocks (bounded, <= ~100 ms) until samples arrive,
//    so the calling loop needs no clock of its own.
//  - Free-running sources (signal generator, file playback) synthesize any
//    amount on demand: read() always fills n immediately and the caller must
//    pace consumption with a real-time clock, exactly as Pipeline already
//    paces SigGen.
//
// Threading: start/stop/setters are called from the GUI or control thread;
// read() only ever from the pipeline's source thread. Implementations
// document any further constraints.
class IqSource {
public:
    virtual ~IqSource() = default;

    // Begin producing (open the device / file). False on failure, with the
    // reason retrievable via lastError(). Idempotent when already running.
    virtual bool start() = 0;
    virtual void stop() = 0;  // idempotent
    virtual bool running() const = 0;

    virtual bool selfPaced() const = 0;

    virtual double sampleRateHz() const = 0;
    // False if the rate is fixed (file) or the device refused it.
    virtual bool setSampleRateHz(double hz) = 0;

    // Center frequency is nominal for sources with no physical tuner (the
    // generator, files): they store and report it so the display stays
    // coherent, and return true.
    virtual double centerFrequencyHz() const = 0;
    virtual bool setCenterFrequencyHz(double hz) = 0;

    // Pull up to n samples into dst; returns the count delivered. 0 means
    // "nothing available yet" for self-paced sources (caller retries) and is
    // never returned by free-running sources.
    virtual std::size_t read(std::complex<float>* dst, std::size_t n) = 0;

    // Short human-readable identity for the Source menu ("Signal generator",
    // "IQ file: capture.wav", "SoapySDR: B200").
    virtual const char* name() const = 0;

    // Last failure description, empty string when none. Never nullptr.
    virtual const char* lastError() const = 0;
};

}  // namespace cascade::source
