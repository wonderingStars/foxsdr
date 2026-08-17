// Demodulator suite: the classic analog voice/CW modes behind one switchable
// object, composed from the existing primitives (QuadDemod, Nco, FirDecimator)
// rather than re-deriving any of them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <string>
#include <vector>

#include "dsp/fir.hpp"
#include "dsp/nco.hpp"
#include "dsp/quad_demod.hpp"

namespace cascade::dsp {

enum class DemodMode { NFM, WFM, AM, DSB, USB, LSB, CW, RAW };

// Number of entries in DemodMode. Kept beside the enum so a new mode makes
// every table that claims to cover all of them fail its own test rather than
// silently ending one short.
inline constexpr std::size_t kDemodModeCount = 8;

// THE canonical spelling of each mode, and the only place mode NAMES are
// defined.
//
// Three separate things need to turn a mode into text or back: the config
// store persists the mode by name (so a saved file survives any future enum
// reorder), the GUI's mode buttons carry their own ORDER — which deliberately
// differs from the enum's — and the web API accepts a mode from a browser.
// Three consumers each keeping a private list is how one of them ends up
// accepting a spelling the others reject; the GUI keeps its ordering table,
// but the vocabulary itself lives here.
//
// modeName returns a stable, never-null string. modeFromName is exact and
// case-sensitive: a browser sending "wfm" is a client bug worth reporting, not
// something to guess at.
const char* modeName(DemodMode m);
bool modeFromName(const std::string& name, DemodMode& out);

// One demodulator per VFO. The channel arrives as complex baseband at
// channelRateHz with the wanted signal already centered at DC (the VFO's
// mixer + decimator upstream did the tuning); process() emits real audio at
// the SAME rate, exactly one output sample per input sample. Resampling to
// the sound card and level normalization (Agc) are separate downstream
// stages — this object stays a pure, testable per-sample map.
//
// Per-mode methods (the details and coefficient derivations live in demod.cpp):
//   NFM  quadrature discriminator (QuadDemod, gain 1): output is the
//        instantaneous frequency in radians/sample, so amplitude is
//        proportional to deviation by construction.
//   WFM  NFM plus the broadcast-FM 75 us one-pole deemphasis.
//   AM   envelope |x| followed by a one-pole DC blocker that removes the
//        carrier's envelope mean.
//   DSB  coherent product detector: with the carrier centered at DC the
//        detector's multiply is by 1, leaving Re{x}.
//   USB/LSB  Weaver-style selection: mix the wanted sideband to straddle DC
//        with an Nco BFO, low-pass with a real-tap FIR (symmetric passband),
//        mix back, take the real part.
//   CW   USB with the input pre-shifted so a carrier on the VFO beats at
//        700 Hz.
//   RAW  Re{x} passthrough, for diagnostics.
class Demodulator {
public:
    Demodulator(double channelRateHz);

    // Selects the mode and resets ALL internal state (filter histories,
    // oscillator phases, one-pole states). A mode switch starts a new stream:
    // carrying state across would inject a transient that belongs to neither
    // mode, so the first block after setMode() must match a fresh object.
    void setMode(DemodMode m);
    DemodMode mode() const { return mode_; }

    // Demodulates n channel samples into n audio samples (1:1 rate; no
    // decimation here). Returns n. State carries across calls, so any block
    // split of a stream produces identical output to one big call.
    std::size_t process(const std::complex<float>* in, std::size_t n, float* out);

    // Clears internal state while keeping the mode; equivalent to
    // setMode(mode()).
    void reset();

    // WFM de-emphasis time constant, in MICROSECONDS. Broadcast FM applies a
    // pre-emphasis curve at the transmitter and the receiver must undo it with
    // the matching one: 50 us across Europe, Africa, Asia and Australia,
    // 75 us in the Americas and South Korea. Using the wrong one is not
    // cosmetic — the audio comes out audibly bright and hissy. 0 disables the
    // filter entirely (useful for measurement, and for feeding an external
    // decoder that wants flat audio). Default is 50 us.
    void setDeemphasisUs(double us);
    double deemphasisUs() const { return deemphTauSec_ * 1.0e6; }

private:
    void processSsb(const std::complex<float>* in, std::size_t n, float* out);

    double rate_;
    DemodMode mode_ = DemodMode::NFM;

    // NFM/WFM discriminator. Gain 1 keeps the output in a physical unit
    // (radians/sample); loudness normalization is the downstream Agc's job.
    QuadDemod quad_{1.0f};

    // WFM deemphasis one-pole: y[n] = (1-p)*x[n] + p*y[n-1]. Pole and state
    // kept in double so the filter matches its analytic transfer function to
    // well below any audio-relevant error.
    double deemphTauSec_ = 0.0;  // 0 = de-emphasis disabled (pole 0 = passthrough)
    double deemphPole_ = 0.0;
    double deemphState_ = 0.0;

    // AM DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1].
    double dcPole_ = 0.0;
    double dcPrevIn_ = 0.0;
    double dcPrevOut_ = 0.0;

    // SSB/CW Weaver chain: BFO down-mix, sideband-select low-pass (decimation
    // 1 = plain streaming FIR), BFO up-mix.
    Nco shiftDown_;
    Nco shiftUp_;
    FirDecimator ssbFilter_;
    std::vector<std::complex<float>> work_;      // down-mixed block
    std::vector<std::complex<float>> filtered_;  // filter output (n+1 capacity)
};

}  // namespace cascade::dsp
