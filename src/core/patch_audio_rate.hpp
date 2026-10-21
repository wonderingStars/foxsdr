// patch_audio_rate.hpp - the rate every patch audio destination is written at, as data (0.99.84 merge, the fence).
//
// Moved out of core/patch_audio.hpp, unchanged: master's 0.99.64 gave patch_audio.hpp the recorder (the WAV destination's slow-disk
// seam is a Recorder::Opener), and the recorder is the engine's, which the interface may not include. The window draws this number.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_AUDIO_RATE_HPP
#define CASCADE_CORE_PATCH_AUDIO_RATE_HPP

namespace cascade::core::patch {

// The rate every destination is written at: the product's audio rate, and one
// MPEG-1 Layer III supports.
inline constexpr double kOutRateHz = 48000.0;

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_AUDIO_RATE_HPP
