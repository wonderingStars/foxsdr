// spectrum_frame.hpp - one published spectrum frame, as data (engine extraction stage 4, the fence).
//
// Moved out of core/pipeline.hpp, unchanged: the window holds the frame it last drew (the browser's copy of it is published from there)
// and the pipeline is the engine's, which the interface may not include. Pipeline names it back.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SPECTRUM_FRAME_HPP
#define CASCADE_CORE_SPECTRUM_FRAME_HPP

#include <cstdint>
#include <vector>

namespace cascade::core {

struct SpectrumFrame {
    std::vector<float> dbBins;   // fftshifted dB power spectrum, size == fftSize
    std::uint64_t seq = 0;       // strictly increasing per published frame
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_SPECTRUM_FRAME_HPP
