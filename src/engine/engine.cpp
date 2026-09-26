// engine.cpp - see engine.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include "engine/receiver_tables.hpp"

namespace cascade::engine {

// The helpers moved out of src/gui keep their namespace, cascade::gui, so the
// code that moved with them reads exactly as it did (docs/engine-stage3.md,
// OPEN: the namespace rename is a mechanical follow-up).
using namespace cascade::gui;

// The Pipeline is built exactly as the window built it: the same rate, FFT
// length and smoothing (engine/receiver_tables.hpp), audio on.
Engine::Engine()
    : pipeline_(cascade::core::Pipeline::Config{kSampleRateHz, kFftSize, kAveragingAlpha,
                                                /*audioEnabled=*/true}) {}

Engine::~Engine() = default;

}  // namespace cascade::engine
