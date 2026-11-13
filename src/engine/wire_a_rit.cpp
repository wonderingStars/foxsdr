// wire_a_rit.cpp - WIRING LANE wire-a, second pass: RIT, the receive clarifier (see core/rit_xit.hpp and the operation's comment in core/wire_a_ops.hpp). Receive only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

WireA::Result WireA::ritOp(const FoxCommand& c, Result res) {
    const std::size_t slot = e_.scopedSlot();
    if (slot >= rit_.size()) { return refused(res, FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    auto& m = rit_[slot].model;
    const double before = m.receiveShiftHz();
    switch (c.ival[0]) {
        case 0: {
            const double hz = c.num[0];
            if (!std::isfinite(hz) || std::fabs(hz) > m.maxHz()) { return refused(res, FOXAPI_OUT_OF_RANGE, "the clarifier offset is within the limit (9999 Hz unless it was changed)"); }
            if (c.ival[1] != 0 && c.ival[1] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[1] is 1 on or 0 off"); }
            if (m.rit() == hz && m.ritOn() == (c.ival[1] == 1)) { return refused(res, FOXAPI_NO_CHANGE, "the clarifier is that already"); }
            m.setRit(hz);
            m.setRitOn(c.ival[1] == 1);
            break;
        }
        case 1: {
            if (!std::isfinite(c.num[0])) { return refused(res, FOXAPI_OUT_OF_RANGE, "the step is a number of hertz"); }
            m.stepRit(c.num[0]);   // clamped to the limit
            m.setRitOn(true);
            break;
        }
        case 2:
            if (m.rit() == 0.0 && !m.ritOn()) { return refused(res, FOXAPI_NO_CHANGE, "the clarifier is clear already"); }
            m.clearRit();
            m.setRitOn(false);
            break;
        case 3: {
            if (!std::isfinite(c.num[1]) || c.num[1] < 0.0 || c.num[1] > 1.0e9) { return refused(res, FOXAPI_OUT_OF_RANGE, "the clear-on-retune distance is 0 (never) or a number of hertz"); }
            if (!m.setMaxHz(c.num[0])) { return refused(res, FOXAPI_OUT_OF_RANGE, "the largest offset is 1 to 20000 Hz"); }
            m.setClearOnRetuneHz(c.num[1]);
            break;
        }
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 set, 1 step, 2 clear or 3 limits");
    }
    e_.rx().pipeline_.setRitHz(m.receiveShiftHz());
    res.applied[0] = m.rit();
    (void)before;
    bump();
    return res;
}

// Once a pass, for every live radio: a retune larger than the clear limit clears the clarifier (the model decides; a limit of 0 never does).
void WireA::ritPass() {
    for (std::size_t slot = 0; slot < rit_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) {
            rit_[slot] = Rit();   // a radio that is gone takes its clarifier with it
            continue;
        }
        Rit& r = rit_[slot];
        Engine::RadioWork work(e_, slot);
        const double hz = e_.currentAbsoluteHz();
        if (!(hz > 0.0) || !std::isfinite(hz)) { continue; }
        if (r.seen && r.model.onVfoMoved(r.seenHz, hz)) {
            e_.rx().pipeline_.setRitHz(0.0);
            bump();
        }
        r.seenHz = hz;
        r.seen = true;
    }
}

void WireA::ritFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    for (std::size_t slot = 0; slot < rit_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        const auto& m = rit_[slot].model;
        const std::string k = "rit." + std::to_string(slot + 1) + ".";
        fact(k + "on", m.ritOn() ? "1" : "0");
        fact(k + "hz", std::to_string(static_cast<long long>(std::llround(m.rit()))));
        fact(k + "max", std::to_string(static_cast<long long>(std::llround(m.maxHz()))));
        fact(k + "clearOnRetune", std::to_string(static_cast<long long>(std::llround(m.clearOnRetuneHz()))));
    }
}

}  // namespace cascade::engine
