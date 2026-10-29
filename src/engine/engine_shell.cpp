// engine_shell.cpp - the implementation of the fence's holder (engine/engine_shell.hpp; engine extraction stage 4, step 5).
//
// The only file that sees both the narrow header the interface includes and the Engine itself. It has NO METHODS to implement since shell retirement R9
// (2026-10-28): the shell is a non-owning view of the engine the host built (R7), the engine's start and stop are the service's (R8), and the bounded run's
// stepping and measurement are the host's (R9: engine/host_measure.cpp). What is left is the view and rawEngine, which the tests reach the Engine
// through (tests/test_host.hpp); R10 deletes both files. Part of the cascade_shell target (the interface links it; the headless engine does not).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine_shell.hpp"

#include <memory>

#include "engine/engine.hpp"

namespace cascade::engine {

struct EngineShell::Impl {
    explicit Impl(Engine& e) : engine(&e) {}
    Engine* engine;
};

EngineShell::EngineShell() = default;
EngineShell::~EngineShell() = default;

std::unique_ptr<EngineShell> EngineShell::view(Engine& engine) {
    std::unique_ptr<EngineShell> shell(new EngineShell());
    shell->impl_ = std::make_unique<Impl>(engine);
    return shell;
}

Engine& rawEngine(EngineShell& shell) { return *shell.impl_->engine; }

}  // namespace cascade::engine
