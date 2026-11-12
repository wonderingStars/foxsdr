// scope_analysis.hpp - THE SCOPE'S ANALYSIS VIEWS (screen lane B, docs/multi-radio-records/screen-b.md): the audio waterfall (S11), the autocorrelation and the cepstrum of the audio (S17) and the eye diagram of the baseband
// I (S18; the constellation is the tube's own VECTOR key). They read the same DemodFeed the tube draws from (scope_logic.hpp: the audio and I/Q taps) and paint into the same glass buffer; nothing is added to the engine.
//
// Pure functions over plain arrays plus one state object (the waterfall's history picture); the painter draws the 5 x 7 face (capitals, digits, signs) for its few words, so nothing here needs translating.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scope_compositor.hpp"
#include "scope_logic.hpp"

namespace foxsdr::slintscope {

// Normalised autocorrelation r[k] = sum x[i] x[i+k] / sum x[i]^2 of the mean-removed signal, k = 0 .. maxLag (r[0] = 1; all zeros for silence). `n` samples are used.
void autocorrelation(const float* x, std::size_t n, std::size_t maxLag, std::vector<float>& out);
// The first strong peak of r after its first fall below zero (a repeating pattern's period in samples; 0 = none standing over `minHeight`).
std::size_t autocorrelationPeak(const std::vector<float>& r, float minHeight = 0.2f);
// The real cepstrum of the audio from its power spectrum in dB (bins 0 .. M-1 of an N = 2M point transform): c[n] for n = 0 .. maxQ, in units of the spectrum's log amplitude. A harmonic series with spacing f0 shows
// as a peak at n = rate / f0.
void cepstrum(const std::vector<float>& spectrumDb, std::size_t maxQ, std::vector<float>& out);
// The index of the strongest cepstral peak in [qLo, qHi] and whether it stands `minAbove` over the mean of the range (0 = none).
std::size_t cepstrumPeak(const std::vector<float>& c, std::size_t qLo, std::size_t qHi, float minAbove = 0.02f);

enum class Analysis { None = 0, Waterfall = 1, Autocorrelation = 2, Cepstrum = 3, Eye = 4 };

struct AnalysisState {
    std::vector<Rgba> history;         // the waterfall's picture, newest line at the top
    int hw = 0, hh = 0;
    int lines = 0;                     // lines added since the picture was (re)made
    std::vector<float> db, r, c, win;  // scratch
    // what the last paint read out (the tests and the drive script read them)
    double peakLagMs = 0, peakHz = 0, pitchHz = 0;
    std::size_t eyeTraces = 0;
    std::string title;
};

// Paints the glass for `mode` (1..4) into px (w x h), returns the number of line segments / lines it drew (0 = nothing to show: no signal). `timebaseMsPerDiv` sets the eye's width (two symbols = ten divisions, one symbol = five).
std::size_t paintAnalysis(Rgba* px, int w, int h, Analysis mode, const DemodFeed& feed, double timebaseMsPerDiv, AnalysisState& st);

}  // namespace foxsdr::slintscope
