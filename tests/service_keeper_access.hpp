// service_keeper_access.hpp - THE ENGINE SERVICE'S KEEPER, FOR A TEST THAT DRIVES IT BY HAND (shell retirement R8; docs/engine-stage4.md 6.25).
//
// The settings file's keeper (the debounced save, the writer and its back-off) was the window's until R8 and the engine service's own copy of it ran beside
// it; the window's was deleted. Two tests drove the WINDOW's: tests/test_failure_files.cpp (a read-only settings folder - a simulated hour of refused writes, the
// back-off, the exit save - and a damaged file that could not be kept aside) and tests/test_health_app.cpp (a refused settings write counted as recovered once). They
// drive the SERVICE's now, through this friend: the same calls, the same clock handed in, the same assertions.
//
// The service is the test host's (th::Host::service()); a window test holds a th::TestWindow, which is an AppWindow, so svcOf(window) finds it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <string>
#include <utility>

#include "core/config.hpp"
#include "core/config_writer.hpp"
#include "engine/engine_service.hpp"
#include "test_host.hpp"

namespace cascade::engine {

struct EngineServiceTestAccess {
    // The writer: bound to a function of the test's (a refusal, a count of attempts), or the real one.
    static void bindWriter(EngineService& s, cascade::gui::ConfigWriter::Writer w) { s.writer_.bind(std::move(w)); }
    static bool inFlight(EngineService& s) { return s.writer_.inFlight(); }
    static bool lastOk(EngineService& s) { return s.writer_.lastOk(); }
    static std::string lastError(EngineService& s) { return s.writer_.lastError(); }
    static unsigned completed(EngineService& s) { return s.writer_.completed(); }
    static bool drain(EngineService& s, std::chrono::milliseconds bound) { return s.writer_.finishOrAbandon(bound); }
    static bool forbidden(EngineService& s) { return s.writer_.writesForbidden(); }
    // The keeper's steps, with the clock handed in (the service's own is its steady clock; a test that wants an hour in a few seconds passes its own).
    static void maybeSave(EngineService& s, double nowS) { s.maybeSave(nowS); }
    static void pollWriter(EngineService& s) { s.pollWriter(); }
    // What the keeper last asked the writer to write (its lastRequested_): the document "the file holds or will".
    static cascade::core::AppConfig lastRequested(EngineService& s) { return s.lastRequested_; }
    // A save that must be current (the final one): the engine's half published, then requested.
    static void saveNow(EngineService& s) { s.saveNow(); }
    // A request of a document of the test's own (what test_health_app asked of the window's writer).
    static void requestSave(EngineService& s, const cascade::core::AppConfig& cfg) { s.requestSave(cfg); }
};

}  // namespace cascade::engine

namespace svc {

// The service behind a test window (th::TestWindow's host).
inline cascade::engine::EngineService& of(cascade::gui::AppWindow& window) { return static_cast<th::TestWindow&>(window).host().service(); }

}  // namespace svc
