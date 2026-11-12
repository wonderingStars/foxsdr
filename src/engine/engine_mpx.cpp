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

}  // namespace cascade::engine
