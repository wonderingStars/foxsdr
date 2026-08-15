// Broadcast-FM stereo (MPX) decoder: 19 kHz pilot PLL, coherent 38 kHz
// subcarrier regeneration, L/R matrixing and per-channel de-emphasis.
//
// Implemented from the published broadcast-FM stereo multiplex standard
// (pilot-tone system) and ordinary PLL/DSP theory — no reference to any other
// receiver's source was used, deliberately, because this tree ships under MIT.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fir.hpp"
#include "dsp/nco.hpp"

namespace cascade::dsp {

// Stereo decoder for the FM broadcast multiplex ("composite", "MPX") signal —
// i.e. the output of the WFM discriminator, BEFORE de-emphasis.
//
// The composite carries three things stacked in frequency:
//
//     0 .. 15 kHz   the mono sum  (L+R), transmitted flat
//        19 kHz     the stereo pilot, a low-level tone
//    23 .. 53 kHz   the difference (L-R), DSB-SC modulated onto a SUPPRESSED
//                   38 kHz subcarrier (= exactly twice the pilot frequency,
//                   and phase-locked to it)
//
// A mono receiver simply low-passes the composite at 15 kHz and hears (L+R);
// that backward compatibility is the whole reason the difference channel is
// hidden on a suppressed carrier up at 38 kHz. The cost is that the receiver
// must REGENERATE that carrier, and DSB-SC demodulation is only correct when
// the regenerated carrier is phase-coherent with the transmitter's:
//
//     (L-R)*cos(w*t) * 2*cos(w*t + e)  --low-pass-->  (L-R)*cos(e)
//
// so a carrier phase error e does not merely attenuate the difference channel,
// it destroys SEPARATION. After matrixing with an unattenuated sum channel,
//
//     L_out = ((L+R) + (L-R)cos e)/2 ,  R_out = ((L+R) - (L-R)cos e)/2
//
// which for a hard-left input (L=1, R=0) leaks (1-cos e)/2 into the right
// channel: separation = 20*log10((1+cos e)/(1-cos e)). That is 40 dB at
// e = 6.4 degrees, 25 dB at e = 27 degrees, and 0 dB at e = 90 degrees. An
// INDEPENDENT free-running 38 kHz oscillator therefore cannot work at any
// accuracy: even a 0.01 Hz frequency error walks e through a full circle in
// under two minutes, so separation would cyclically collapse to nothing and
// the image would swap channels. This is why the standard sends a pilot at all
// — the 38 kHz must be derived from it by frequency doubling, which preserves
// phase (doubling a phase-locked 19 kHz gives a phase-locked 38 kHz), and that
// is exactly what this class does: ONE phase accumulator, locked to the pilot,
// whose doubled phase drives the product detector.
//
// Signal flow (all at the composite rate; nothing is resampled here):
//
//   mpx --+------------------------------------------------> [ 15 kHz LPF ] --> (L+R)
//         |                                                        ^
//         +--> x e^-j2pi*19k --> [narrow LPF] --> pilot baseband   |
//         |                            |                          |
//         |                        [ PLL ] --> phase Phi           |
//         |                            |                          |
//         +--> x 2cos(2*Phi) ----------+------------------------> [ 15 kHz LPF ] --> (L-R)
//
//   L = ((L+R) + g*(L-R))/2 ,  R = ((L+R) - g*(L-R))/2 ,  then de-emphasis
//
// where g is a smoothly ramped stereo gate (see setForceMono / pilotLocked).
//
// The two 15 kHz low-passes are ONE FirDecimator: the sum path rides in the
// real part and the difference path in the imaginary part of a single complex
// stream. The taps are real, so the two parts are filtered independently and
// identically — which also guarantees the two paths have bit-identical
// magnitude and delay responses, and matrix errors from filter mismatch (a
// classic source of poor separation) cannot exist by construction.
//
// Scaling: the decoder is a pure matrix, so it reproduces whatever scaling the
// modulator used. A standard-compliant composite carries 0.45*(L+R) and
// 0.45*(L-R), so a hard-left tone of amplitude 1 comes out at 0.45 on the left
// channel. Absolute loudness is the downstream AGC's job, exactly as for the
// other demodulators in this tree.
//
// Thread-safety: none needed — like every block in src/dsp this is a plain
// object owned by one thread.
class StereoFm {
public:
    // compositeRateHz is the sample rate of the discriminator output feeding
    // process() — typically 192-250 kHz for a WFM channel.
    //
    // The composite occupies DC..53 kHz, so full stereo decoding needs at
    // least 106 kHz. Below that the (L-R) sidebands are not present in the
    // input at all and stereoCapable() reports false: the object still works,
    // but permanently as a mono decoder (pilot machinery disabled, L == R).
    explicit StereoFm(double compositeRateHz);

    // Decodes n composite samples into n left and n right samples at the same
    // rate; returns n. State carries across calls, so any block split of a
    // stream produces bit-identical output to one big call.
    //
    // `left` and `right` must be distinct buffers and must not alias `mpx`.
    std::size_t process(const float* mpx, std::size_t n, float* left,
                        float* right);

    // Forgets all stream state — filter histories, oscillator and PLL phase,
    // level estimates, the stereo gate and the de-emphasis memories — so the
    // next process() behaves like a freshly constructed object. SETTINGS
    // (force-mono, de-emphasis time constant) are preserved: they are user
    // choices, not stream state.
    void reset();

    // True when a 19 kHz pilot is present and the PLL is tracking it. Drives
    // the "ST" indicator and, internally, the stereo gate.
    bool pilotLocked() const noexcept { return locked_; }

    // Pilot strength indicator in [0, 1] for the UI, defined as the amplitude
    // of the PHASE-COHERENT pilot component divided by the amplitude a
    // standard pilot would have for the current composite level. It reads ~1
    // for any standards-compliant stereo broadcast (loud or quiet passage
    // alike, because the reference tracks the composite), falls off with a
    // weak pilot, and reads ~0 on a mono broadcast or on noise — noise in the
    // pilot band averages to zero in the coherent estimator even though its
    // raw energy does not.
    float pilotLevel() const noexcept { return level_; }

    // User override: forces L = R = mono regardless of the pilot. Takes effect
    // through the same smooth gate as an ordinary loss of lock, so toggling it
    // mid-stream is click-free.
    void setForceMono(bool on) noexcept { forceMono_ = on; }
    bool forceMono() const noexcept { return forceMono_; }

    // De-emphasis time constant in MICROSECONDS, applied to BOTH channels
    // AFTER matrixing — which is where the standard puts it, because the
    // transmitter pre-emphasises L and R individually before the matrix. (Put
    // before the matrix here it would also, wrongly, shape the pilot and the
    // 38 kHz sidebands.) 50 us is the standard outside the Americas/South
    // Korea and is the default; 75 us is the Americas value; 0 disables it,
    // which is what a measurement or an external decoder wants.
    void setDeemphasisUs(double us);
    double deemphasisUs() const noexcept { return deemphTauSec_ * 1.0e6; }

    // False when compositeRateHz is too low to carry the (L-R) sidebands; the
    // object is then a permanent mono decoder. See the constructor comment.
    bool stereoCapable() const noexcept { return stereoCapable_; }

private:
    double rate_;
    bool stereoCapable_;
    bool forceMono_ = false;

    // --- pilot recovery -----------------------------------------------------
    // Fixed oscillator at -19 kHz: mixing the (real) composite with it brings
    // the pilot to DC, where a plain real-tap low-pass becomes a narrow
    // COMPLEX band-pass around 19 kHz. Doing it this way rather than with a
    // real band-pass FIR has two advantages: the phase detector then sees no
    // double-frequency (38 kHz) ripple at all, because the negative-frequency
    // image was removed by the complex mix rather than merely filtered; and
    // the same oscillator supplies the deterministic part of the regenerated
    // carrier phase (see stereo_fm.cpp), so the two can never disagree.
    Nco pilotNco_;
    FirDecimator pilotLpf_;
    // Group delay of pilotLpf_ in samples: the PLL therefore measures the
    // pilot phase as it was pilotGroupDelay_ samples ago, and the loop's own
    // frequency estimate is used to extrapolate it back to "now".
    double pilotGroupDelay_ = 0.0;

    // --- audio path ---------------------------------------------------------
    // ONE 15 kHz low-pass for both outputs: the sum path travels in the real
    // part and the difference path in the imaginary part. Real taps filter the
    // two parts independently, so this is two filters' worth of work with one
    // filter's worth of state — and, more importantly, the two paths cannot
    // drift apart in gain or delay, which is the classic way a stereo matrix
    // loses separation.
    FirDecimator audioLpf_;

    // --- pilot PLL ----------------------------------------------------------
    // Second-order (type-2) loop: the integrator makes the STEADY-STATE phase
    // error zero for a constant frequency offset, which a type-1 loop cannot
    // do — and per the class comment, static phase error is exactly what
    // destroys separation. State is the RESIDUAL phase/frequency relative to
    // the nominal 19 kHz grid, so both are ~0 for an on-frequency pilot.
    //
    // These are bare accumulators rather than an Nco on purpose: Nco's
    // contract is a CONSTANT frequency advanced over a whole block, whereas a
    // loop rewrites its phase and frequency on every single sample from the
    // detector output. Splitting the oscillator this way — Nco for the fixed
    // 19 kHz grid, a raw accumulator for the residual the loop controls — lets
    // the deterministic part keep Nco's exact wrapping and drift-free phase
    // while the feedback part stays where feedback belongs.
    double psi_ = 0.0;   // radians
    double freq_ = 0.0;  // radians per sample
    double kp_ = 0.0;
    double ki_ = 0.0;
    double freqMax_ = 0.0;  // capture/hold limit, radians per sample

    // --- level, lock and the stereo gate ------------------------------------
    double levelAlpha_ = 0.0;  // one-pole coefficient for all level estimates
    double compSq_ = 0.0;      // smoothed mean square of the composite
    double cohI_ = 0.0;        // smoothed pilot baseband, de-rotated by the PLL
    double cohQ_ = 0.0;
    float level_ = 0.0f;
    bool locked_ = false;
    double gate_ = 0.0;      // stereo blend, 0 = mono, 1 = full difference
    double gateStep_ = 0.0;  // per-sample ramp increment

    // --- de-emphasis (one per channel, applied after the matrix) ------------
    double deemphTauSec_ = 0.0;
    double deemphPole_ = 0.0;
    double deemphLeft_ = 0.0;
    double deemphRight_ = 0.0;

    // Scratch, kept as members so steady-state process() calls never allocate.
    std::vector<std::complex<float>> osc_;
    std::vector<std::complex<float>> mix_;
    std::vector<std::complex<float>> pilotBb_;
    std::vector<std::complex<float>> audioIn_;
    std::vector<std::complex<float>> audioOut_;
};

}  // namespace cascade::dsp
