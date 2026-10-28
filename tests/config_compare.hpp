// config_compare.hpp - THE SAVE DEBOUNCE'S COMPARE, FOR A TEST (shell retirement R8; docs/engine-stage4.md 6.25).
//
// Until R8 the window kept the settings file itself and decided "has anything changed" with a hand-kept, field-by-field comparison (gui::configsEqual,
// about 240 lines in app_window.cpp), and tests/test_config.cpp and the window tests asked it directly - one field at a time - because a field missing from
// that list was a setting that reached the file only by accident. The file is kept by the engine's service now, and its compare is EngineService::comparable:
// the whole serialised document, minus the ten fields that move by themselves (core/config_halves.hpp, clearBookkeeping). A field a later build adds to
// AppConfig is compared by being written, so the failure the hand-kept list could not rule out cannot happen - and every assertion those tests made,
// "this change is seen by the debounce", is the same assertion about the production compare. This header gives them the old spelling over it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "core/config.hpp"
#include "engine/engine_service.hpp"

namespace cfgcmp {

// True when the keeper would see NO difference between `a` and `b` (they would not cause a save).
inline bool configsEqual(const cascade::core::AppConfig& a, const cascade::core::AppConfig& b) {
    return cascade::engine::EngineService::comparable(a) == cascade::engine::EngineService::comparable(b);
}

}  // namespace cfgcmp
