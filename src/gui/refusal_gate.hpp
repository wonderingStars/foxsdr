// refusal_gate.hpp - THE ONE PLACE the classic window decides whether the "this radio is a patch part's" popup may be shown at all (0.99.113).
//
// Today it always may. It is a function of its own so that a remote feature word can switch the popup off later by changing this one body; nothing else in the window decides it. (The Source
// line's sentence is not behind this gate: it is the engine's, and every client reads it.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::gui {

inline bool mayShowRefusalPopup() { return true; }

}  // namespace cascade::gui
