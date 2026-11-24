// radio_refusal_gate.hpp - THE ONE PLACE the new window decides whether the "this radio is a patch part's" popup may be shown at all (0.99.113).
//
// It may unless another user of the one Handover sheet (the radio hand-off question, the Legacy-window question: slint/handover_sheet.hpp) has it - the popup never takes the sheet from either.
// It is a function of its own so that a remote feature word can switch the popup off later by changing this one body; nothing else in the window decides it. (The Source
// line's sentence is not behind this gate: it is the engine's, and every client reads it.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace foxsdr::slinthost {

inline bool mayShowRefusalPopup(bool sheetFree) { return sheetFree; }

}  // namespace foxsdr::slinthost
