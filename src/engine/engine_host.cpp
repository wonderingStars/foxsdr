// engine_host.cpp - the headless answers to EngineHost (engine_host.hpp): what
// an engine with no window gets. Clocks from std::chrono, measured from the
// PROCESS'S first host (see processEpoch: one origin for every EngineHost, so that
// an engine whose host was replaced (Engine::setHost, R7 to R8; gone) read one continuous
// clock); nothing to redraw; no store window, so no ADD ALL plan.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine_host.hpp"

#include <utility>

#include "core/tune_control.hpp"

namespace cascade::engine {

namespace {

// THE ONE ORIGIN OF EVERY HOST'S CLOCK (shell retirement R7). A host used to measure from its own construction, which was harmless while an
// engine kept one host for life. The window application WAS the engine's SECOND host (R7b to R8b; Engine::setHost, removed by R9: the service restored on its own
// host, the window installed its adapter before the control thread started), and the usage report stamps the session's start at the restore and
// reads the session's length - and the mode seconds - later: two origins would put those stamps and reads on different clocks. Every
// EngineHost therefore answers from the moment the process built its first one.
std::chrono::steady_clock::time_point processEpoch() {
    static const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    return epoch;
}

}  // namespace

EngineHost::EngineHost() : born_(processEpoch()) {}
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
