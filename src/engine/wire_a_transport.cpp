// wire_a_transport.cpp - WIRING LANE wire-a, second pass: FILE_TRANSPORT, the speed, pause and step of a radio that plays a recorded I/Q file (see source/iq_file_source.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"
#include "source/iq_file_source.hpp"

namespace cascade::engine {

namespace {
cascade::source::IqFileSource* fileOf(Receiver& r) {
    cascade::source::IqFileSource* f = dynamic_cast<cascade::source::IqFileSource*>(&r.pipeline_.rawSource());
    return f != nullptr && f->totalFrames() > 0 ? f : nullptr;
}
}  // namespace

WireA::Result WireA::transportOp(const FoxCommand& c, Result res) {
    cascade::source::IqFileSource* f = fileOf(e_.rx());
    if (f == nullptr) { return refused(res, FOXAPI_UNSUPPORTED, "this radio is not playing an I/Q file"); }
    using F = cascade::source::IqFileSource;
    switch (c.ival[0]) {
        case 0: {
            const double s = c.num[0];
            if (!std::isfinite(s) || s < F::kMinPlaySpeed || s > F::kMaxPlaySpeed) { return refused(res, FOXAPI_OUT_OF_RANGE, "the speed is 0.25 to 8"); }
            if (s == f->playSpeed()) { return refused(res, FOXAPI_NO_CHANGE, "the file plays at that speed already"); }
            f->setPlaySpeed(s);
            res.applied[0] = s;
            break;
        }
        case 1: {
            if (c.ival[1] != 0 && c.ival[1] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "the policy is 0 varispeed or 1 block skip"); }
            if ((c.ival[1] == 1) == f->speedBlockSkip()) { return refused(res, FOXAPI_NO_CHANGE, "that policy is in force already"); }
            f->setSpeedBlockSkip(c.ival[1] == 1);
            break;
        }
        case 2:
            if (f->playbackPaused()) { return refused(res, FOXAPI_NO_CHANGE, "the file is paused already"); }
            f->pausePlayback();
            break;
        case 3:
            if (!f->playbackPaused()) { return refused(res, FOXAPI_NO_CHANGE, "the file is playing"); }
            f->resumePlayback();
            break;
        case 4: {
            if (!f->playbackPaused()) { return refused(res, FOXAPI_DENIED, "a step needs the file paused"); }
            const long long n = c.ival[1];
            if (n == 0 || n > F::kMaxStepBlocks || n < -F::kMaxStepBlocks) { return refused(res, FOXAPI_OUT_OF_RANGE, "a step is 1 to 4096 blocks forward or back"); }
            if (!f->stepBlocks(n)) { return refused(res, FOXAPI_DENIED, "the step was not accepted"); }
            res.applied[0] = static_cast<double>(n);
            break;
        }
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 speed, 1 policy, 2 pause, 3 resume or 4 step");
    }
    bump();
    return res;
}

void WireA::transportFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    for (std::size_t slot = 0; slot < 8; ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        cascade::source::IqFileSource* f = fileOf(*e_.receivers_[slot]);
        if (f == nullptr) { continue; }
        const std::string k = "playback." + std::to_string(slot + 1) + ".";
        char b[32];
        std::snprintf(b, sizeof(b), "%g", f->playSpeed());
        fact(k + "speed", b);
        fact(k + "policy", f->speedBlockSkip() ? "blockskip" : "varispeed");
        fact(k + "paused", f->playbackPaused() ? "1" : "0");
    }
}

}  // namespace cascade::engine
