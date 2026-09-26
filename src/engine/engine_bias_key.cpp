// engine_bias_key.cpp - the Engine's machinery from gui/app_window_bias_key.cpp, moved VERBATIM (engine
// extraction stage 3a, docs/engine-stage3.md): each definition is the
// window's, renamed AppWindow:: -> Engine::, with the few lines that reached
// into the window turned into calls on the host (engine_host.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include "core/i18n.hpp"
#include "engine/bias_tee.hpp"

#include "engine/audio_open.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine/rate_follow_status.hpp"
#include "engine/receiver_tables.hpp"
#include "engine/running_view.hpp"
#include "engine/soundcard_panel.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tune_control.hpp"
#include "engine/tx_frequency.hpp"


namespace cascade::engine {

// The helpers moved out of src/gui keep their namespace (cascade::gui), so
// the code that moved with them reads exactly as it did.
using namespace cascade::gui;
using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// The stand-in's identity: a serial-named radio, so the "not asked again"
// half of the gate can be seen in a capture as well as the first question.
constexpr const char* kStandInKind = "stand-in";

constexpr const char* kStandInArgs = "serial=5A4E0001";

}  // namespace

bool Engine::biasTeeReachable() const {
    // biasTeePanel_.present is written after each OPEN and by nothing else, so
    // on its own it outlives the radio: a HackRF closed for the generator left
    // it true (test_bias_key_app [5] caught the key still drawn over the
    // generator). The Source panel's checkbox never showed that, because it is
    // drawn inside the open device's own panel; the deck has no such frame. So
    // the key asks the question live - a radio IS open, it is still answering,
    // and withBiasTee still reaches a bias tee on it (an RSPdx answers per
    // antenna).
    //
    // A RADIO THAT HAS STOPPED ANSWERING HAS NO KEY (repair round 1, F3). Its
    // driver refuses every transfer from then on, so a key over it could only
    // ever be pressed for nothing, and its lamp would be a readback the radio
    // can no longer confirm; the FAIL lamp beside it and the Source panel are
    // where a dead radio is reported, and the reopen that follows a driver
    // fault puts the key back with the radio.
    return biasTeePanel_.present && device_ != nullptr && !device_->deviceDead() &&
           withBiasTee(device_, [](auto&) { return true; });
}

std::string Engine::biasKeyRadioNow() const {
    if (biasStandInActive()) { return biasKeyRadio(kStandInKind, kStandInArgs); }
    return biasKeyRadio(sourceKind_, deviceArgs_);
}

bool Engine::biasKeyMayRememberNow() const {
    // The stand-in is a serial-named radio with an unbounded memory of one.
    if (biasStandInActive()) { return biasKeyMayRemember(kStandInArgs); }
    // A real radio: exactly when an "on" switched now will be kept and put
    // back at its next open (review round 2, L2) - which is also when the
    // session may skip the question next time (the gate's mayRemember).
    return device_ != nullptr && biasTeeWillRestoreOn(biasTeePanel_, *device_, deviceArgs_);
}

void Engine::switchBiasTee(bool want) {
    if (biasStandInActive()) {
        // A stand-in driver: it takes the change, or refuses it and says so
        // exactly where a real driver's refusal is shown.
        if (biasStandIn_ == BiasStandIn::Accept) {
            biasStandInOn_ = want;
        } else {
            sourceError_ = "stand-in bias tee (FOXSDR_FORCE_BIAS_KEY=refuse): switching the "
                           "bias-T was refused";
        }
        return;
    }
    // The checkbox's own path, unchanged: request, then show the READBACK; a
    // refusal leaves the box, the key and the memory where they were.
    std::string err;
    biasTeeTicked(biasTeePanel_, device_, deviceArgs_, want, &err);
    if (!err.empty()) { sourceError_ = err; }
}

}  // namespace cascade::engine
