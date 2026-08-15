// IqSource adapter around SigGen: the Pipeline's built-in, hardware-free
// source. Free-running per the IqSource pacing contract (selfPaced() ==
// false): read() always synthesizes and fills the full request immediately,
// and the CALLER supplies the real-time pacing — exactly how the Pipeline's
// source thread already paced SigGen before this abstraction existed, so
// wrapping the generator changes nothing observable.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>

#include "source/iq_source.hpp"
#include "source/siggen.hpp"

namespace cascade::source {

class SigGenSource final : public IqSource {
public:
    explicit SigGenSource(double sampleRateHz);

    // The underlying generator, exposed so the existing tone/noise
    // configuration surface (Pipeline::sigGen()) keeps working unchanged.
    // SigGen's own threading rules apply: configuring slots while read()
    // runs on the source thread is the same benign concurrency the Pipeline
    // has always allowed ("configure tones before/while running").
    SigGen& sigGen();

    // Trivial lifecycle: there is no device to open, so start() cannot fail
    // and stop() has nothing to tear down. Both are idempotent per contract.
    bool start() override;
    void stop() override;
    bool running() const override;

    bool selfPaced() const override;  // false — free-running by definition

    double sampleRateHz() const override;
    // Refused: the rate is fixed at construction because the Pipeline's DSP
    // chain (ring sizing, VFO, resampler) is built around it and SigGen
    // normalized its tone frequencies against it — accepting a new rate here
    // would silently detune every configured slot. The IqSource contract
    // allows a fixed-rate source to answer false.
    bool setSampleRateHz(double hz) override;

    // Nominal only (no physical tuner): stored and reported so the display
    // stays coherent, per the IqSource contract. Defaults to 100 MHz.
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;

    // Always fills n for a valid request — a free-running source never
    // returns 0 to mean "nothing available yet".
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    const char* name() const override;       // "Signal generator"
    const char* lastError() const override;  // always "" — nothing can fail

private:
    SigGen gen_;
    double sampleRateHz_;
    // Plain (non-atomic) members are sufficient: the IqSource threading
    // contract routes start/stop/setters and the getters through the GUI or
    // control thread only, and read() — the only source-thread entry point —
    // never touches them.
    double centerHz_ = 100.0e6;
    bool running_ = false;
};

}  // namespace cascade::source
