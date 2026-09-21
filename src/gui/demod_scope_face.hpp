// demod_scope_face.hpp - the tube, the bezel, the graticule and the beam.
//
// SEPARATE FROM demod_scope.hpp ON PURPOSE, and for exactly the reason
// scope_face.hpp is separate from scope_view.hpp: that header is the scope's
// ARITHMETIC and is deliberately free of ImGui, which is what lets
// tests/test_demod_scope.cpp exercise the trigger, the ladders and the
// reductions without a graphics context. Anything needing an ImDrawList lives
// here.
//
// THE FACE DRAWS THE PICTURE AND NOTHING ELSE. Every control - the four signal
// keys, the time base, the attenuator, the AUTO latch - is a real ImGui item
// drawn by the page in app_window.cpp, so it takes part in the same input
// arbitration, focus order and keyboard reach as every other control on the
// bench. A hand-drawn thing inside a draw list that only LOOKS clickable is
// what scope_face.hpp's own header forbids, and the same rule holds here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_DEMOD_SCOPE_FACE_HPP
#define CASCADE_GUI_DEMOD_SCOPE_FACE_HPP

#include <cstddef>

#include "gui/demod_scope.hpp"
#include "imgui.h"

namespace cascade::gui {

// WHAT THE PAGE HANDS THE TUBE. Plain pointers and counts rather than
// containers: the page owns the buffers and reuses them frame to frame, and
// nothing here allocates.
struct DemodScopeFeed {
    // The demodulated audio, oldest first, as taken from the pipeline's scope
    // tap. Already the sweep-plus-search window the page asked for.
    const float* audio = nullptr;
    std::size_t audioCount = 0;
    double audioRateHz = 0.0;

    // The channel I/Q at the demodulator's input, DE-INTERLEAVED by the page
    // into two parallel arrays of `iqCount` samples each.
    //
    // TWO ARRAYS RATHER THAN THE complex<float> THE TAP CARRIES, because the
    // reductions this face runs want a stride of one, and splitting them here
    // would mean allocating a scratch buffer inside a draw call - once per
    // frame, for a hundred thousand samples. The page owns that buffer and
    // reuses it; the face allocates nothing at all.
    const float* iqI = nullptr;
    const float* iqQ = nullptr;
    std::size_t iqCount = 0;
    double iqRateHz = 0.0;

    // The audio spectrum, already transformed by the page (which owns the FFT
    // plan) and already cut to the span the axis is ruled for: magnitudes in
    // dB, bin 0 at DC, one bin every `spectrumBinHz`. The face draws exactly
    // these bins across the whole width, so what is on the glass and what the
    // readout says the span is cannot disagree.
    const float* spectrumDb = nullptr;
    std::size_t spectrumBins = 0;
    double spectrumBinHz = 0.0;
    // The axis the bins were cut to, in hertz. Derivable from the two above
    // to within one bin, and passed anyway because the MULTIPLEX labels are
    // placed against it: a pilot rule that lands a bin's width away from the
    // pilot is a drawing that disagrees with its own axis.
    double spectrumSpanHz = 0.0;

    // WHOSE SOUND THIS IS. Empty for the demodulated audio; the plugin's name
    // when one has taken the speakers through CASCADE_CAP_AUDIO_OUT. The tap
    // sits BELOW that handover, so the trace is genuinely the plugin's audio -
    // and a scope that did not say so would be attributing a DAB programme to
    // the FM discriminator.
    const char* audioFrom = nullptr;

    // Whether the chain is producing at all. False draws the graticule and NO
    // BEAM, with the reason lettered on the tube: a flat line at zero volts
    // says "we measured, and it is silent", which is a different statement
    // from "nothing is arriving" and this product has a standing rule against
    // conflating the two.
    bool live = false;
};

// DRAW THE WHOLE TUBE into `dl` between tl and br: the bay, the bezel, the
// phosphor ground, the graticule, the beam and the three readouts along the
// bottom of the glass.
//
// SCRATCH IS THE CALLER'S. `lo` and `hi` must each hold at least `scratchCap`
// floats and are used for the per-column envelope; the page keeps them as
// members so a frame of scope costs no allocation. A caller that passes fewer
// than a handful of columns' worth simply gets a coarser trace.
void drawDemodScopeFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                        const DemodScopeFeed& feed, const DemodScopeState& state,
                        float* lo, float* hi, std::size_t scratchCap);

// THE PEAK THE AUTO RANGING IS FED, for whichever signal is selected: the
// largest audio sample for the two audio positions, the largest |I|, |Q| for
// the two baseband ones. Here rather than in the face because the page applies
// it to its own state - the face draws, it does not decide - and here rather
// than in demod_scope.hpp because it reaches into the feed.
//
// Zero when there is nothing to measure, which scopeAutoGain reads as "hold
// the current step" rather than as "wind it all the way in".
float demodScopePeak(const DemodScopeFeed& feed, ScopeSignal signal);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_DEMOD_SCOPE_FACE_HPP
