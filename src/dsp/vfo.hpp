// VFO channelizer: NCO mix to baseband, anti-alias low-pass, integer decimate.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fir.hpp"
#include "dsp/nco.hpp"

namespace cascade::dsp {

// Extracts one channel from a wideband complex stream: mixes the spectrum so
// that (input center + offset) lands at DC, low-passes to the channel
// bandwidth, and decimates to the channel rate. This class composes the
// existing Nco and FirDecimator — it owns only tuning and filter-design
// policy, never the DSP inner loops, so the numerical guarantees of those
// blocks (phase-exact NCO, bit-exact streaming decimation) carry over.
class Vfo {
public:
    // channel rate = inputRateHz / decimation. Designs the anti-alias
    // low-pass for bandwidthHz. The bandwidth is clamped into
    // [0.01, 0.9] * channelRate: the upper bound keeps a real transition band
    // between the passband edge and the channel Nyquist (otherwise no finite
    // filter could prevent aliasing), the lower bound keeps the windowed-sinc
    // design well-posed (cutoff must stay strictly positive).
    // Preconditions: inputRateHz > 0, decimation >= 1.
    Vfo(double inputRateHz, unsigned decimation, double bandwidthHz);

    // Tuning offset in Hz from the input center frequency; takes effect at
    // the next process() call. The NCO retains its accumulated phase across
    // the change, so the mixed stream is phase-continuous — no discontinuity
    // click beyond the inherent frequency step itself. Offsets beyond
    // +/- inputRate/2 fold back (they alias in a sampled mixer regardless).
    void setOffsetHz(double offset);

    double offsetHz() const;

    // Redesigns the anti-alias filter for the new bandwidth (same clamping as
    // the constructor) and CLEARS the filter history and the decimation grid
    // phase: the next process() behaves like a fresh stream through the new
    // filter. History cannot be meaningfully carried across a tap-count
    // change, so a documented clean restart beats a pretend-seamless one.
    // The NCO phase and the offset are not touched.
    void setBandwidthHz(double bw);

    double channelRateHz() const;

    // Mix down by the offset, filter, decimate. Streaming: filter history,
    // NCO phase, and the decimation grid all persist across calls, so any
    // block segmentation of the input yields identical output samples.
    // Returns the number of channel samples written to out (at most outCap).
    // outCap should be >= n / decimation + 1 (the worst case for any grid
    // phase); if it is smaller, the surplus outputs of THIS call are dropped
    // but the stream state still advances over all n inputs, so later calls
    // stay consistent.
    std::size_t process(const std::complex<float>* in, std::size_t n,
                        std::complex<float>* out, std::size_t outCap);

    // Zeroes the NCO phase and the filter history/grid. Offset, bandwidth,
    // and the designed filter are retained: the next process() behaves like a
    // freshly constructed Vfo with the same settings.
    void reset();

private:
    double clampBandwidth(double bw) const;
    std::vector<float> designTaps(double bw) const;

    double inputRate_;
    unsigned decimFactor_;
    double bandwidth_;  // after clamping
    double offset_ = 0.0;
    Nco nco_;
    FirDecimator decimator_;
    // Scratch buffers, members so steady-state process() calls don't
    // allocate; sized to the internal chunk, not to the caller's n.
    std::vector<std::complex<float>> mixBuf_;
    std::vector<std::complex<float>> chanBuf_;
};

}  // namespace cascade::dsp
