// real_to_iq.hpp - one channel of REAL audio (a sound card's line input)
// turned into complex I/Q the pipeline can use, with the spectrum on the true
// air-frequency axis.
//
// WHAT A SOUND CARD HEARS AND WHAT THE PIPELINE WANTS. A sound card sampling
// at fs delivers a real signal: everything from 0 to fs/2, and a mirror image
// of it at the negative frequencies that carries no information of its own.
// Fed to the pipeline as it stands (I = the audio, Q = 0) the display would
// span -fs/2 .. +fs/2 around a centre of 0 Hz, every station would appear
// twice, and a 17.2 kHz transmitter would show up at both +17.2 and -17.2 kHz.
//
// SO THE NEGATIVE HALF IS REMOVED AND THE POSITIVE HALF IS CENTRED, which is
// the textbook route to an analytic signal at half the rate:
//
//   1. multiply by e^(-j*pi*n/2) - a shift of -fs/4, so 0 .. fs/2 on the air
//      becomes -fs/4 .. +fs/4, and the mirror image lands on the outer
//      quarters. The sequence is 1, -j, -1, +j, so the "mixer" is four sign
//      and swap cases rather than a sine table;
//   2. low-pass at fs/4 (a windowed-sinc half-band) to remove the mirror;
//   3. keep every second sample: complex rate fs/2.
//
// The result at fs/2 complex, CENTRED ON fs/4, covers exactly 0 .. fs/2 of
// the air: at 192 kHz the display spans 0 - 96 kHz on a 48 kHz centre, a
// 17.2 kHz tone appears at 17.2 kHz, and a USB receiver tuned to 16.4 kHz
// hears it as 800 Hz. tests/test_soundcard_source.cpp measures all three.
//
// THE COST, AND WHERE IT FALLS. The filter's transition band sits at the two
// edges of the display (air 0 Hz and air fs/2), which is also where a sound
// card's own DC blocking and anti-alias filter already are, so nothing usable
// is lost that the card had not already thrown away. kTaps = 255 with a
// Blackman-Harris window gives a transition about 0.016 fs either side of the
// edge (3 kHz at 192 kHz) and a mirror suppressed by more than 90 dB inside it.
//
// AMPLITUDE. A real cosine of amplitude A is two half-amplitude complex tones,
// and step 2 keeps one of them, so the output is scaled by 2: a full-scale
// sine on the card is a full-scale complex tone, exactly as on an I/Q input.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fir.hpp"

namespace cascade::dsp {

class RealToIq {
public:
    // Odd, so the half-band has integer group delay (see windowedSincLowpass).
    static constexpr std::size_t kTaps = 255;

    RealToIq();

    // The complex rate and the centre that a real input at inputRateHz comes
    // out at. Both are exact halves/quarters: every rate a sound card offers
    // is an even integer, so these are integers too.
    static double outputRateHz(double inputRateHz) { return inputRateHz / 2.0; }
    static double centreHz(double inputRateHz) { return inputRateHz / 4.0; }

    // At most this many outputs from process(in, n, out), for any phase: n
    // real samples contain at most ceil(n / 2) points of the decimation grid.
    static std::size_t outputCapacity(std::size_t n) { return n / 2 + 1; }

    // Converts n real samples; returns the complex outputs written. The mixer
    // phase and the filter history carry across calls, so any split of the
    // input into blocks gives bit-for-bit the outputs of one big call - and n
    // real samples starting anywhere contain AT MOST ceil(n/2) outputs, so a
    // caller that hands in 2m samples gets exactly m.
    std::size_t process(const float* in, std::size_t n, std::complex<float>* out);

    // Forget the history and restart the mixer at phase 0.
    void reset();

private:
    FirDecimator fir_;
    unsigned phase_ = 0;  // index into 1, -j, -1, +j
    std::vector<std::complex<float>> mixed_;
};

}  // namespace cascade::dsp
