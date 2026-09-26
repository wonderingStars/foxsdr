// receiver_tables.hpp - the receiver's fixed tables and constants, shared by
// the Engine (which runs the receiver) and the window (which draws it).
//
// Moved verbatim out of gui/app_window.cpp's file-local namespace (engine
// extraction stage 3a): once the machinery that reads them lives in
// src/engine, one copy has to be reachable from both sides of the line, and a
// table written twice is two tables that will one day disagree. Kept in
// namespace cascade::gui, like the other helpers that moved, so every caller
// reads exactly as before.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_RECEIVER_TABLES_HPP
#define CASCADE_ENGINE_RECEIVER_TABLES_HPP

#include <cstddef>

namespace cascade::gui {

// Pipeline configuration. 2 MS/s at FFT 1024 publishes far more frames than
// the GUI's ~60 fps polls; the latest-frame slot in Pipeline absorbs the
// difference by design. Alpha 0.5 smooths the trace without visible lag.
constexpr double kSampleRateHz = 2'000'000.0;
constexpr std::size_t kFftSize = 1024;
constexpr float kAveragingAlpha = 0.5f;

}  // namespace cascade::gui

#endif  // CASCADE_ENGINE_RECEIVER_TABLES_HPP
