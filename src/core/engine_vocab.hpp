// engine_vocab.hpp - the three constants the source combo's rows and the decoder log are counted with (shell retirement R10, 2026-10-29).
//
// Plain data the engine and its front end both speak. They were the first lines of "the vocabulary" in the shell header (engine_shell.hpp, deleted), which the window
// included for them (the fence, stage 4 step 5); the shell is deleted, so they live with the other data headers in core/, where the interface may
// include them and the engine names them back (engine.hpp: `using cascade::core::kSoundCardRow` ..., Engine::kSoundCardRow and kNativeRowBase and
// kDecoderLogMax are these).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>

namespace cascade::core {

// WHERE EACH FAMILY'S ROWS START IN THE SOURCE COMBO. Row 0 is the generator, row 1 the IQ file and row 2 the sound card; the native
// radios come next, and the SoapySDR devices after them (Engine::kSoundCardRow and kNativeRowBase are these).
inline constexpr int kSoundCardRow = 2;
inline constexpr int kNativeRowBase = 3;
// The decoder output's bound: the panel is a tail, not an archive (Engine::kDecoderLogMax is this).
inline constexpr std::size_t kDecoderLogMax = 500;

}  // namespace cascade::core
