// host_measure.hpp - A BOUNDED RUN'S HOST HARNESS: the stepping of the engine's pass and the measurement run, over the engine (shell retirement R9,
// 2026-10-28; docs/engine-stage4.md 6.25).
//
// A bounded run (`--frames N`) is a test or a tool. Two things about it are the HOST's, not the window's, because the host owns the engine: the
// pass is STEPPED (the engine's thread runs one part of a pass for each call the window makes at the three places of its frame where the pump ran
// them, so the run is deterministic and frame-exact - 65 gui tests and most of the window tests are written against that), and the MEASUREMENT RUN
// (FOXSDR_MEASURE, tools/measure_engine.ps1) installs a custom I/Q source, reads the pipeline and the pass's figures and takes one step a frame.
// Neither is a table call: a table op returns at once and a step blocks, and a custom source is an object.
//
// makeBoundedHooks builds the two std::functions of core/bounded_hooks.hpp the window is handed (WindowHost::bounded). main() calls it when
// boot.frames >= 0 and never otherwise; the test host calls it for the same condition; a product run has no hooks, its pass free-runs and the window
// steps nothing (tests/test_engine_shell_guard.cpp holds both). Also here, because it is a bounded run's and the harness's: the CASCADE_PLUGIN_TEST
// diagnostic's arming and the frame it names (Engine::setPluginTestReport, notePluginTestFrame) - the window's frame, told at the end of each frame.
//
// Engine-clean for the gui: this header is in src/engine and the interface may not include it (cmake/interface_fence.cmake); the window sees only
// core/bounded_hooks.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <memory>

#include "core/bounded_hooks.hpp"
#include "core/engine_measure.hpp"

namespace cascade::engine {

class Engine;

// The hooks for a bounded run over `engine` (which must outlive them): stepPass runs one part of the engine's pass and waits for it; onFrame tells the
// engine which frame the window has reached and, when `measure` is set, takes the measurement's one step (false once it has finished). Reads
// CASCADE_PLUGIN_TEST (a bounded run only, which is the only run this is called for) and arms the catalogue report when it names an address.
cascade::core::BoundedHooks makeBoundedHooks(Engine& engine, std::unique_ptr<cascade::core::EngineMeasure> measure = nullptr);

}  // namespace cascade::engine
