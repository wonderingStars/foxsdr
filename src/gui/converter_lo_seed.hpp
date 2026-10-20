// converter_lo_seed.hpp - when the Converter section's local-oscillator field is re-seeded from the engine's stored setting.
//
// THE PUMP MOVE, step P2. The engine used to tell the window "the converter changed" (EngineHost::onConverterChanged, called from
// applyConverterForSource and changeConverter - on the engine's pass) and the window's hook cleared the field's seed so the next
// draw re-seeded it and, for a radio change, cleared the red "could not read" note. The hook wrote two window fields from the
// engine's thread; a pass on a thread of its own must not. The window already READS everything the hook announced - the open
// radio's key (fact.converter.radioKey) and the stored setting (the CONVERTERS entry that is ACTIVE) - so the seed is derived from
// them: whenever the radio, or any part of its stored setting, differs from what the field was last seeded from, it is seeded
// again. While the user types and nothing under it changes, what they type stays.
//
// What the hook caught that this does not: a converter command that changed NOTHING (the same mode, the same LO, the same
// inversion) used to re-seed the field too, which tidied typed-but-uncommitted text back to the stored figure; the field now keeps
// that text until the user commits or the radio or the setting really changes. The window's own tidy (a commit that equals the
// stored LO clears the seed itself) is unchanged.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "core/freq_converter.hpp"

namespace cascade::gui {

// The key the LO field was seeded from: the radio, and its stored setting - the LO as the field prints it, the mode, the inversion.
inline std::string converterLoSeed(const std::string& radioKey, const cascade::core::ConverterSetting& stored) {
    std::string seed = radioKey;
    seed += '\x1f';
    seed += cascade::core::converterHzText(stored.loHz);
    seed += '\x1f';
    seed += std::to_string(static_cast<int>(stored.mode));
    seed += stored.inverted ? 'i' : 'n';
    return seed;
}

}  // namespace cascade::gui
