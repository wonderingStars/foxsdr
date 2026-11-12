// engine_vfo.cpp - VFO A / B for a receive-only program (engine serial lane A, row T6).
//
// WHAT A "VFO" IS HERE. The radio already has one tune: its frequency, mode and bandwidth. That tune belongs to the ACTIVE letter (Receiver::vfoActive_, A by default); the OTHER letter is
// a register (Receiver::vfoOther_) that remembers a frequency, a mode and a bandwidth and is never heard until it is brought in. Bringing it in is vfoExchange: the radio's tune goes into the
// register, the register's tune becomes the radio's, and the radio retunes (the mode first, because a mode change sets the mode's default bandwidth; then the frequency, by the counter's own
// path, so the band follow, the decoders' forgetting and the lock all act as they do for any retune; then the bandwidth the register held). The two letters swap places in one step and nothing
// is lost: swapping twice gives back what was there.
//
// WHAT IS NOT HERE: SPLIT. Split means "listen on A, transmit on B"; a receive-only program has no second frequency to use, so there is no split flag. The model (core/operating_aids.hpp VfoPair)
// keeps its split for the transmit side.
//
// Used by FOXAPP_OP_VFO_SELECT and FOXAPP_OP_VFO_SWAP in Engine::applyCommand, under the addressed radio's scope.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include "core/receiver_tables.hpp"

namespace cascade::engine {

void Engine::vfoExchange() {
    Receiver& r = rx();
    const Receiver::VfoReg live{currentAbsoluteHz(), r.modeIndex_, r.vfoBandwidthHz_, true};
    // A register that was never set is the radio's own tune: bringing in "B" the first time changes the letter and nothing else.
    const Receiver::VfoReg target = r.vfoOther_.set ? r.vfoOther_ : live;
    r.vfoOther_ = live;
    if (target.mode != r.modeIndex_) { setModeIndex(target.mode); }
    if (target.hz != live.hz) { tuneAbsoluteHz(target.hz); }
    if (target.bwHz > 0.0 && target.bwHz != r.vfoBandwidthHz_) {
        r.vfoBandwidthHz_ = target.bwHz;
        applyChannelBandwidth();
        r.bandwidthIndex_ = cascade::gui::bandwidthStepIndex(r.vfoBandwidthHz_);
    }
}

}  // namespace cascade::engine
