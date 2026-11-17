// s4b_macro_edit.hpp - screen lane S4b, G3-34: saving and deleting the macros that engine/engine_macros.cpp plays (the model is core/macros.hpp).
//
// Macros played from a hand-edited macros.json; nothing made one. FOXAPP_OP_MACRO_EDIT (0x8A22) takes JSON {"action":"save","name":"...","description":"...","steps":[{"cmd":"SET_FREQUENCY","num":[Hz],
// "ival":[...],"delayMs":N}]} (replacing a macro of that name) or {"action":"delete","name":"..."}. The file is read, changed, checked WHOLE by the model's own validation (an unknown or denied command, a
// bad number, too many steps, a name that is empty or has a control character: the macro is refused whole, never half-saved) and written through a temporary file and a rename; the engine then reads it
// again. A macro never runs because it was saved: it runs when the person asks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

namespace cascade::engine::s4b {

// Returns the FOXAPI status; `why` carries the sentence of a refusal. `path` is the macro file (it need not exist yet).
std::int32_t macroEdit(const std::string& path, const std::string& text, std::string& why);

}  // namespace cascade::engine::s4b
