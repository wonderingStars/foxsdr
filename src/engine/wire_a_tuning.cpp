// wire_a_tuning.cpp - WIRING LANE wire-a, the tuning group: frequency history (HISTORY_STEP, fact.history.<n>.*).
//
// FREQUENCY HISTORY (core/freq_history.hpp, row 9 of the survey): every radio keeps its own 16-entry back / forward list of (absolute frequency, mode). The list is fed
// ONCE A PASS from the radio's tuned frequency, not from each tune path: a digit wheel, a VFO drag, a bookmark, a band key, the scanner, a plugin's preset and a CAT
// command all end in a different frequency, and the pass sees that - so no tune path has to know the history exists. The model coalesces a dial spin into one entry and
// ignores the retune its own back / forward causes (this file also records the frequency the radio read back after a step, so a device that coerces the tune by a few Hz
// does not read as a new entry). In memory only: a history that outlives the session would be a list of places the user listened.
//   HISTORY_STEP ival[0] 0 = back, 1 = forward, on the selected radio (target): tunes it to the entry and sets the mode the entry was heard in. NO_CHANGE at an end
//   of the list; any other ival[0] is OUT_OF_RANGE; a radio with no source open cannot tune: FAILED.
//   fact.history.<n>.count, .position (from 0), .canBack, .canForward: for each live radio n.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstring>
#include <string>

#include "core/app_commands.hpp"
#include "core/receiver_tables.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace {
int modeIndexOf(const std::string& name) {
    for (int m = 0; m < 8; ++m) {
        if (name == cascade::gui::kModeNames[m]) { return m; }
    }
    return -1;
}
}  // namespace

WireA::Result WireA::historyStep(const FoxCommand& c, Result res) {
    const std::size_t slot = e_.scopedSlot();
    if (slot >= history_.size()) { return refused(res, FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    if (c.ival[0] != 0 && c.ival[0] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "0 is back and 1 is forward"); }
    History& h = history_[slot];
    const bool back = c.ival[0] == 0;
    if (back ? !h.list.canBack() : !h.list.canForward()) {
        return refused(res, FOXAPI_NO_CHANGE, back ? "there is nothing further back in the history" : "there is nothing further forward in the history");
    }
    const auto entry = back ? h.list.back() : h.list.forward();
    if (!entry.has_value()) { return refused(res, FOXAPI_NO_CHANGE, "the history is empty"); }
    // The mode first (a retune may snap to the mode's grid), then the frequency; the readback is what the pass must not take for a new retune.
    const int mode = modeIndexOf(entry->mode);
    if (mode >= 0 && mode != e_.rx().modeIndex_) { e_.setModeIndex(mode); }
    e_.tuneAbsoluteHz(entry->hz);
    h.seenHz = e_.currentAbsoluteHz();
    h.seen = true;
    res.applied[0] = h.seenHz;
    res.applied[1] = static_cast<double>(h.list.position());
    bump();
    return res;
}

void WireA::historyPass() {
    const double nowMs = e_.clockS() * 1000.0;
    for (std::size_t slot = 0; slot < history_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        Engine::RadioWork work(e_, slot);
        History& h = history_[slot];
        const double hz = e_.currentAbsoluteHz();
        if (!(hz > 0.0) || !std::isfinite(hz)) { continue; }   // no source yet: nothing to record
        if (h.seen && std::fabs(hz - h.seenHz) < 0.5) { continue; }
        h.seenHz = hz;
        h.seen = true;
        const int m = e_.rx().modeIndex_;
        if (m < 0 || m >= 8) { continue; }
        if (h.list.push(hz, cascade::gui::kModeNames[m], nowMs)) { bump(); }
    }
}

void WireA::historyFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    for (std::size_t slot = 0; slot < history_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        const std::string k = "history." + std::to_string(slot + 1) + ".";
        const History& h = history_[slot];
        fact(k + "count", std::to_string(h.list.size()));
        fact(k + "position", std::to_string(h.list.position()));
        fact(k + "canBack", h.list.canBack() ? "1" : "0");
        fact(k + "canForward", h.list.canForward() ? "1" : "0");
    }
}

}  // namespace cascade::engine
