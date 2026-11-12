// engine_hotplug.cpp - HOT-PLUG DETECTION (engine serial lane A, row R10): a radio plugged in or pulled out is noticed.
//
// WHAT IS WIRED. core/sys_device_watch.hpp (the standalone half, from the sysmodels lane) is a message-only window on a thread of its own that receives WM_DEVICECHANGE for the two USB device-interface
// classes the engine enumerates, parses each path to a vendor id, a product id and a class (the serial number is dropped the moment it is read), and coalesces a re-plug's burst of events into ONE
// callback after a quiet period. This file owns one watcher per engine and acts on that callback on the control pass:
//   * the native device list is scanned again (it opens nothing) and the SoapySDR scan is asked for through its own gate (it defers while a radio is open and says so once);
//   * when something ARRIVED and a saved radio is waiting out a busy retry (Engine::pollSavedRadioRetry), the retry is brought forward to this pass with its attempts reset: a radio that was busy or
//     not there a moment ago is tried the moment a device appears;
//   * the facts fact.hotplug.on, .bursts, .arrivals, .removals count what happened (counts only: no name, no serial, no path is kept, logged or reported).
// WHAT IS NOT: a radio that is open and is pulled out is still found dead by its driver, as before, and is not re-opened by this file (the descriptor of a radio that died is not kept anywhere
// the retry could use it: an owner decision, listed in docs/multi-radio-records/engine-serial-a.md).
//
// THREADS. The watcher's callback runs on its window thread and touches atomics only; the work is done by pumpHotplug on the control pass.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_log.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

void Engine::startHotplugWatch(bool registerWithOs, cascade::core::sys::ArrivalConfig cfg) {
    if (hotplugWatch_) { return; }
    hotplugWatch_ = std::make_unique<cascade::core::sys::DeviceArrivalWatcher>(cfg, [this](const cascade::core::sys::ArrivalSummary& s) {
        hotplugArr_.fetch_add(s.arrivals, std::memory_order_relaxed);
        hotplugRem_.fetch_add(s.removals, std::memory_order_relaxed);
        hotplugSettled_.store(true, std::memory_order_release);
    });
    if (!hotplugWatch_->start(registerWithOs)) {
        cascade::core::diagLogf("hot-plug: the device-change notice could not be started");
        hotplugWatch_.reset();
        return;
    }
    ++hotplugVersion_;
}

void Engine::stopHotplugWatch() {
    if (!hotplugWatch_) { return; }
    hotplugWatch_->stop();
    hotplugWatch_.reset();
    ++hotplugVersion_;
}

void Engine::pumpHotplug() {
    if (!hotplugSettled_.load(std::memory_order_acquire)) { return; }
    hotplugSettled_.store(false, std::memory_order_relaxed);
    const int arrived = hotplugArr_.exchange(0, std::memory_order_relaxed);
    const int removed = hotplugRem_.exchange(0, std::memory_order_relaxed);
    ++hotplugBursts_;
    hotplugArrivalsTotal_ += arrived;
    hotplugRemovalsTotal_ += removed;
    ++hotplugVersion_;
    cascade::core::diagLogf("hot-plug: %d arrived, %d removed", arrived, removed);   // counts only, never a name
    scanNative();
    scanSoapy();
    if (arrived > 0 && rx().savedRadioRetryBusy_) {
        // a device appeared while a saved radio is waiting: try it now (the retry's own rules - the same radio kinds, the cap on attempts - still apply)
        rx().savedRadioRetryAttempts_ = 0;
        rx().savedRadioRetryLastFailSec_ = -1.0e9;
    }
}

}  // namespace cascade::engine
