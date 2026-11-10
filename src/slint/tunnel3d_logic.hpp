// tunnel3d_logic.hpp - THE I/Q TUNNEL'S DATA AND WORDS (docs/engine-stage4.md 6.64, specs/THREED.md section 4): the demod scope's baseband I/Q samples laid out along a time axis, the TIME DEPTH
// choices, the axis range and its number formats. Pure: no Slint, no engine, no window.
//
// THE DATA IS THE DEMOD SCOPE'S: DemodScope's feed (scope_logic.hpp) carries the channel's baseband I/Q the scope's I/Q and VECTOR inputs draw; the publisher copies it here. NOTHING ELSE
// IS INVENTED: the design's EVM, SNR, SYMBOL RATE and LOCK readouts and its COLOUR BY Symbol need a demodulator's symbol clock and decisions, which the host does not have (the scope sees
// samples, not symbols), so they are not drawn and are listed in docs 6.64 as deferred. COLOUR BY offers Time and Level (the sample's distance from the middle), both true to the samples.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace foxsdr::slintview3d {

constexpr std::array<double, 16> kTunnelDepthsMs{2.0, 4.0, 6.0, 8.0, 10.0, 15.0, 20.0, 30.0, 40.0, 60.0, 80.0, 100.0, 120.0, 150.0, 200.0, 250.0};   // TIME DEPTH's sixteen positions
constexpr int kTunnelDefaultDepth = 8;                                                                                                             // 40 ms, the design's

// What the demod scope hands the 3D view each tick.
struct TunnelFeed {
    bool shown = false;                  // the demod window is open on a baseband input (I/Q or VECTOR)
    bool live = false;                   // samples are arriving
    std::vector<float> i, q;             // the newest samples, oldest first, as many as the tap holds
    double rateHz = 0.0;                 // the samples' rate
    std::string dead;                    // the scope's own sentence when there is nothing to draw ("NO SAMPLES - RECEIVER STOPPED")
    std::string caption;                 // the scope's own caption of the input ("DEMODULATED I/Q", "VECTOR I-Q"), translated
};

// The time the tap's samples span, in milliseconds (0 for no rate).
double availableMs(std::size_t samples, double rateHz);
// TIME DEPTH index 0..15 -> milliseconds, never more than `availMs` (a depth the tap cannot fill shows what it has) and never under one sample.
double effectiveDepthMs(int index, double availMs, double rateHz);
// The samples to draw for a depth: the newest ones, at least 2 and at most `have`.
std::size_t samplesFor(double depthMs, double rateHz, std::size_t have);
// The axis half range for samples of peak `peak` (largest |I| or |Q|): the smallest of 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10 that is at least 1.05 * peak; 1 for a peak of 0 or less.
double niceAxis(double peak);
// "0.25", "-1.00", "0.00": two decimals (one for a half range of 5 or more).
std::string formatAxis(double v, double axis);
// "0", "12.5", "40": whole milliseconds, one decimal under a 10 ms span.
std::string formatMs(double ms, double spanMs);
// The slider's 0..1 and the index, both ways.
int depthIndexOfSlider(float v);
float sliderOfDepthIndex(int index);

}  // namespace foxsdr::slintview3d
