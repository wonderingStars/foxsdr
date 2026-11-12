// engine_mpx.cpp - THE FM MULTIPLEX TAKE (engine serial lane A, row D4): FOXAPP_OP_MPX_RECORD. The tap is Pipeline::setMpxRecorder (the composite, resampled to 192 kHz, handed to a Recorder); this file ends
// the take: the tap is taken out first (after the setter returns no write against the recorder is in flight), then the recorder finalises its file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

namespace cascade::engine {

void Engine::stopMpxRecording() {
    rx().pipeline_.setMpxRecorder(nullptr);
    if (rx().mpxRecorder_.recording()) {
        rx().mpxRecorder_.stop();
        ++mpxVersion_;
    }
}

void Engine::stopIfRecording() {
    rx().pipeline_.setIfRecorder(nullptr);
    if (rx().ifRecorder_.recording()) {
        rx().ifRecorder_.stop();
        ++mpxVersion_;
    }
}

void Engine::pumpPpmAuto() {
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        if (!receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        SlotScope scope(this, slot);
        Receiver& r = rx();
        if (!r.ppmAuto_.active()) { continue; }
        const double now = clockS();
        if (r.ppmAutoLastReadS_ >= 0.0 && now - r.ppmAutoLastReadS_ < 0.1) { continue; }
        r.ppmAutoLastReadS_ = now;
        const bool wfm = r.modeIndex_ == 1;
        r.ppmAuto_.add(now, wfm && r.pipeline_.pilotLocked(), wfm ? static_cast<double>(r.pipeline_.pilotLevel()) : 0.0, r.pipeline_.pilotOffsetHz());
        ++ppmAutoVersion_;
    }
}

cascade::core::Pipeline::ZoomFrameInfo Engine::zoomFrame(std::uint32_t radio, float* out, std::size_t cap) {
    if (!receiverLive(radio)) { return {}; }
    return receivers_[radio - 1u]->pipeline_.zoomFrame(out, cap);
}

}  // namespace cascade::engine
