// engine_host.cpp - the headless answers to EngineHost (engine_host.hpp): what
// an engine with no window gets. Clocks from std::chrono, measured from the
// host's construction; nothing to redraw; no store window, so no ADD ALL plan.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine_host.hpp"

#include <utility>

#include "engine/tune_control.hpp"

namespace cascade::engine {

EngineHost::EngineHost() : born_(std::chrono::steady_clock::now()) {}
EngineHost::~EngineHost() = default;

double EngineHost::wallTimeS() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - born_).count();
}

void EngineHost::pauseWatchdog() {}
void EngineHost::resumeWatchdog() {}

void EngineHost::onCatalogueFetchStarting() {}
void EngineHost::onCatalogueResult() {}
void EngineHost::onAddAllFinished() {}


}  // namespace cascade::engine
