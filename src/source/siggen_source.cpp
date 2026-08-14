// SPDX-License-Identifier: MIT
#include "source/siggen_source.hpp"

namespace cascade::source {

SigGenSource::SigGenSource(double sampleRateHz)
    // SigGen itself guards a nonpositive rate (falls back to 1 Hz); mirror
    // the sanitized value here so sampleRateHz() reports what the generator
    // actually normalizes tones against, not the caller's junk input.
    : gen_(sampleRateHz), sampleRateHz_(sampleRateHz > 0.0 ? sampleRateHz : 1.0) {}

SigGen& SigGenSource::sigGen() {
    return gen_;
}

bool SigGenSource::start() {
    running_ = true;
    return true;
}

void SigGenSource::stop() {
    running_ = false;
}

bool SigGenSource::running() const {
    return running_;
}

bool SigGenSource::selfPaced() const {
    return false;
}

double SigGenSource::sampleRateHz() const {
    return sampleRateHz_;
}

bool SigGenSource::setSampleRateHz(double hz) {
    (void)hz;
    return false;  // fixed at construction — see the header for why
}

double SigGenSource::centerFrequencyHz() const {
    return centerHz_;
}

bool SigGenSource::setCenterFrequencyHz(double hz) {
    centerHz_ = hz;  // nominal: stored for display coherence, always accepted
    return true;
}

std::size_t SigGenSource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) {
        return 0;  // degenerate request, not a "nothing available" signal
    }
    // Free-running: synthesize the whole request on demand. Tone phase and
    // the noise stream carry across calls inside SigGen, so chunking is
    // invisible (bit-exact) to consumers — the property the pipeline's
    // pacing loop relies on.
    gen_.generate(dst, n);
    return n;
}

const char* SigGenSource::name() const {
    return "Signal generator";
}

const char* SigGenSource::lastError() const {
    return "";  // nothing here can fail
}

}  // namespace cascade::source
