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
//
// CHANNEL MODE (core/channel_plans.hpp, row 11): CHANNEL (see core/wire_a_ops.hpp) puts a radio on a numbered plan (PMR446, CB, FRS, GMRS, MURS, marine VHF), steps
// it channel by channel and goes to a channel by its label; each channel retunes the radio, sets the plan's mode hint and the plan's width as the bandwidth. The cursor is per
// radio and in memory (a plan chosen is the user's for the session). Leaving the mode leaves the radio where it is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstring>
#include <string>

#include "core/app_commands.hpp"
#include "core/key_actions.hpp"
#include "core/receiver_tables.hpp"
#include "core/spectrum_frame.hpp"
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

// ---- channel mode ---------------------------------------------------------------------------------------------------------------------------

WireA::Result WireA::channelOp(const FoxCommand& c, const std::string& text, Result res) {
    namespace t = cascade::core::tuning;
    const std::size_t slot = e_.scopedSlot();
    if (slot >= channel_.size()) { return refused(res, FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    t::ChannelCursor& cur = channel_[slot];
    const t::PlanChannel* ch = nullptr;
    switch (c.ival[0]) {
        case 0:
            if (!cur.setPlan(text)) { return refused(res, FOXAPI_OUT_OF_RANGE, "there is no channel plan with that name"); }
            ch = cur.current();
            break;
        case 1:
        case 2:
        case 3:
            if (!cur.active()) { return refused(res, FOXAPI_NO_CHANGE, "no channel plan is chosen"); }
            if (c.ival[0] == 3) {
                if (!cur.goTo(text)) { return refused(res, FOXAPI_OUT_OF_RANGE, "the plan has no channel with that number"); }
                ch = cur.current();
            } else {
                ch = c.ival[0] == 1 ? cur.next() : cur.prev();
            }
            break;
        case 4:
            if (!cur.active()) { return refused(res, FOXAPI_NO_CHANGE, "channel mode is not on"); }
            cur = t::ChannelCursor();
            bump();
            return res;
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 plan, 1 next, 2 previous, 3 go to, 4 leave");
    }
    if (ch == nullptr || cur.plan() == nullptr) { return refused(res, FOXAPI_FAILED, "the plan has no channels"); }
    tuneTo(ch->freqHz, cur.plan()->modeHint, cur.plan()->tuneBandwidthHz());
    res.applied[0] = e_.currentAbsoluteHz();
    bump();
    return res;
}

void WireA::channelFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    std::string plans;
    for (const cascade::core::tuning::ChannelPlan& p : cascade::core::tuning::channelPlans()) {
        if (!plans.empty()) { plans += '\n'; }
        plans += p.id + '\t' + p.name;
    }
    fact("channel.plans", plans);
    for (std::size_t slot = 0; slot < channel_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        const std::string k = "channel." + std::to_string(slot + 1) + ".";
        const cascade::core::tuning::ChannelCursor& cur = channel_[slot];
        const cascade::core::tuning::PlanChannel* ch = cur.current();
        fact(k + "plan", cur.plan() != nullptr ? cur.plan()->id : std::string());
        fact(k + "label", ch != nullptr ? ch->label : std::string());
        fact(k + "freqHz", ch != nullptr ? std::to_string(static_cast<long long>(std::llround(ch->freqHz))) : std::string());
        fact(k + "altHz", ch != nullptr && ch->altHz > 0.0 ? std::to_string(static_cast<long long>(std::llround(ch->altHz))) : std::string());
        fact(k + "note", ch != nullptr ? ch->note : std::string());
        fact(k + "modeHint", cur.plan() != nullptr ? cur.plan()->modeHint : std::string());
        fact(k + "numberingIsOurs", cur.plan() != nullptr && cur.plan()->numberingIsOurs ? "1" : "0");
    }
}

// ---- key actions --------------------------------------------------------------------------------------------------------------------------------
// core/key_actions.hpp's pure handlers on the engine's own state. The tuning step is the window's, so cycle_step is not here.

WireA::Result WireA::keyActionOp(const FoxCommand& c, const std::string& text, Result res) {
    (void)c;
    namespace t = cascade::core::tuning;
    const t::KeyActionInfo* info = t::findKeyAction(text);
    if (info == nullptr) { return refused(res, FOXAPI_OUT_OF_RANGE, "that is not a key action"); }
    Receiver& rxs = e_.rx();
    const double tuned = e_.currentAbsoluteHz();
    switch (info->id) {
        case t::KeyActionId::BandwidthUp:
        case t::KeyActionId::BandwidthDown: {
            const std::string mode = rxs.modeIndex_ >= 0 && rxs.modeIndex_ < 8 ? cascade::gui::kModeNames[rxs.modeIndex_] : "";
            const auto next = t::bandwidthStep(t::defaultBandwidths(mode), rxs.vfoBandwidthHz_, info->id == t::KeyActionId::BandwidthUp ? 1 : -1);
            if (!next.has_value()) { return refused(res, FOXAPI_NO_CHANGE, "this mode has no list of bandwidths"); }
            if (std::fabs(*next - rxs.vfoBandwidthHz_) < 0.5) { return refused(res, FOXAPI_NO_CHANGE, "the bandwidth is at the end of the list"); }
            const Result r = subCommand(FOXAPI_OP_SET_BANDWIDTH, *next, 0);
            res.applied[0] = r.applied[0];
            return res;
        }
        case t::KeyActionId::CycleStep:
            return refused(res, FOXAPI_UNSUPPORTED, "the tuning step is kept by the window");
        case t::KeyActionId::CycleMode: {
            std::vector<std::string> modes;
            for (int m = 0; m < 7; ++m) { modes.push_back(cascade::gui::kModeNames[m]); }   // not RAW
            const std::string cur = rxs.modeIndex_ >= 0 && rxs.modeIndex_ < 8 ? cascade::gui::kModeNames[rxs.modeIndex_] : "";
            const auto next = t::cycleMode(modes, cur, 1);
            if (!next.has_value()) { return refused(res, FOXAPI_NO_CHANGE, "no mode to go to"); }
            for (int m = 0; m < 8; ++m) {
                if (*next == cascade::gui::kModeNames[m]) {
                    e_.setModeIndex(m);
                    res.applied[0] = m + 1;
                    return res;
                }
            }
            return refused(res, FOXAPI_FAILED, "the mode is not known");
        }
        case t::KeyActionId::CentreSignal: {
            cascade::core::SpectrumFrame frame;
            if (!rxs.pipeline_.getLatestFrame(frame) || frame.dbBins.size() < 8) { return refused(res, FOXAPI_NO_CHANGE, "there is no spectrum to look at yet"); }
            const double rate = rxs.pipeline_.inputRateHz();
            const double centre = rxs.pipeline_.activeSource().centerFrequencyHz();
            const double n = static_cast<double>(frame.dbBins.size());
            if (!(rate > 0.0)) { return refused(res, FOXAPI_NO_CHANGE, "the radio has no sample rate yet"); }
            const double half = 0.5 * rxs.vfoBandwidthHz_;
            double bestDb = -1.0e9, bestHz = 0.0;
            bool found = false;
            for (std::size_t i = 0; i < frame.dbBins.size(); ++i) {
                const double hz = centre + (static_cast<double>(i) - n / 2.0) * rate / n;
                if (std::fabs(hz - tuned) > half) { continue; }
                if (frame.dbBins[i] > bestDb) { bestDb = frame.dbBins[i]; bestHz = hz; found = true; }
            }
            if (!found) { return refused(res, FOXAPI_NO_CHANGE, "no signal inside the passband"); }
            const auto to = t::centreOnSignal(tuned, bestHz, half);
            if (!to.has_value()) { return refused(res, FOXAPI_NO_CHANGE, "the signal is centred already"); }
            e_.tuneAbsoluteHz(*to);
            res.applied[0] = e_.currentAbsoluteHz();
            bump();
            return res;
        }
        case t::KeyActionId::NextBookmark:
        case t::KeyActionId::PreviousBookmark: {
            std::vector<double> hz;
            for (const cascade::core::Bookmark& b : e_.freqMgr_.list()) { hz.push_back(b.freqHz); }   // the list is kept sorted ascending
            const auto to = info->id == t::KeyActionId::NextBookmark ? t::nextBookmark(hz, tuned) : t::previousBookmark(hz, tuned);
            if (!to.has_value()) { return refused(res, FOXAPI_NO_CHANGE, "there are no bookmarks"); }
            for (const cascade::core::Bookmark& b : e_.freqMgr_.list()) {
                if (std::fabs(b.freqHz - *to) < 0.5) {
                    e_.tuneToBookmark(b);
                    break;
                }
            }
            res.applied[0] = e_.currentAbsoluteHz();
            bump();
            return res;
        }
    }
    return refused(res, FOXAPI_OUT_OF_RANGE, "that is not a key action");
}

}  // namespace cascade::engine
