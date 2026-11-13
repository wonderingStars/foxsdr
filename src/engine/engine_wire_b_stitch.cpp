// engine_wire_b_stitch.cpp - the engine's side of the stitched sweep of an ordinary radio (wire-b second pass, item 10; the state machine is wire_b_stitch.{hpp,cpp}).
//
// SWEEP_OPEN kind "radio" (engine_wire_b.cpp sweepOp) calls stitchStart(); Engine::pumpWireB calls pumpStitch() once per control pass; SWEEP_CLOSE and a radio that closes call stitchEnd().
// RECEIVE ONLY: the only thing this ever asks of the radio is SET_CENTRE through the ordinary command path (so the record lock, a follower's lock and every other refusal of a retune apply as they
// do for the user), and the radio's own centre is put back when the run ends however it ends. Every caller holds the lane's mutex.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "engine/engine_wire_b.hpp"

namespace cascade::engine {

namespace {
bool readRadio(Receiver& r, cascade::core::PublishedState& ps) { return r.receiverSnapshot_ != nullptr && r.receiverSnapshot_->read(ps); }
}  // namespace

FoxCommandResult Engine::stitchStart(std::size_t slot, const wireb::StitchSettings& s, FoxCommandResult res) {
    WireBState& w = wireBState();
    const auto refuse = [&](std::int32_t status, const char* why) {
        res.status = status;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", why);
        return res;
    };
    if (w.sweep.running()) { return refuse(FOXAPI_BUSY, "a sweep device is open already"); }
    if (slot >= receivers_.size() || !receivers_[slot]) { return refuse(FOXAPI_FAILED, "that radio is not open"); }
    cascade::core::PublishedState ps;
    if (!receivers_[slot]->pipeline_.running() || !readRadio(*receivers_[slot], ps) || !(ps.rx.sampleRateHz > 0.0) || !(ps.rx.centreHz > 0.0)) {
        return refuse(FOXAPI_FAILED, "that radio is not running: start it first");
    }
    std::string why;
    if (!w.stitch.begin(s, ps.rx.sampleRateHz, 0.0, why)) { return refuse(FOXAPI_BAD_ARGUMENT, why.c_str()); }
    if (!w.sweep.beginExternal("radio", s.startHz, s.stopHz, why)) { return refuse(FOXAPI_FAILED, why.c_str()); }
    w.stitchSlot = slot;
    w.stitchOrigCentreHz = ps.rx.centreHz;
    w.stitchRetuned = false;
    w.stitchFrame = cascade::core::SpectrumFrame{};
    w.stitchFrame.seq = 0;
    cascade::core::diagLogf("sweep: a stitched sweep of radio %zu started (%zu centres)", slot + 1, w.stitch.steps());
    return res;
}

void Engine::stitchEnd(const std::string& sentence) {
    if (wireB_ == nullptr) { return; }
    WireBState& w = *wireB_;
    if (w.stitchRetuned && w.stitchSlot < receivers_.size() && receivers_[w.stitchSlot] && w.stitchOrigCentreHz > 0.0) {
        FoxCommand cmd = cascade::core::cmd::makeNum(FOXAPI_OP_SET_CENTRE, w.stitchOrigCentreHz);
        cmd.target = static_cast<std::uint32_t>(w.stitchSlot) + 1u;
        (void)applyCommand(cmd);
    }
    w.stitchRetuned = false;
    w.stitch.cancel();
    w.sweep.endExternal(sentence);
}

void Engine::pumpStitch() {
    WireBState& w = *wireB_;
    if (!w.stitch.active()) { return; }
    const std::size_t slot = w.stitchSlot;
    if (slot >= receivers_.size() || !receivers_[slot]) {
        w.stitch.fail("the radio was closed during the sweep");
        w.stitchRetuned = false;   // (nothing to put back)
        w.sweep.endExternal(w.stitch.failure());
        ++wireBVersion_;
        return;
    }
    Receiver& r = *receivers_[slot];
    wireb::StitchObservation obs;
    cascade::core::PublishedState ps;
    if (readRadio(r, ps)) { obs.centreHz = ps.rx.centreHz; obs.rateHz = ps.rx.sampleRateHz; }
    obs.haveFrame = r.pipeline_.getLatestFrame(w.stitchFrame);   // true only for a frame NEWER than the last one taken
    obs.seq = w.stitchFrame.seq;
    obs.db = &w.stitchFrame.dbBins;
    const double nowS = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    wireb::StitchStep st = w.stitch.advance(nowS, obs);
    if (st == wireb::StitchStep::Retune) {
        FoxCommand cmd = cascade::core::cmd::makeNum(FOXAPI_OP_SET_CENTRE, w.stitch.retuneTo());
        cmd.target = static_cast<std::uint32_t>(slot) + 1u;
        const FoxCommandResult cr = applyCommand(cmd);
        w.stitchRetuned = true;
        if (cr.status != FOXAPI_OK && cr.status != FOXAPI_NO_CHANGE) {
            w.stitch.fail(std::string("the radio refused the retune: ") + cr.message);
            st = wireb::StitchStep::Failed;
        }
    }
    if (st == wireb::StitchStep::LineReady) {
        cascade::core::SweepLine line;
        if (w.stitch.takeLine(line)) { w.sweep.ingestLine(line); }
        ++wireBVersion_;
    } else if (st == wireb::StitchStep::Finished) {
        stitchEnd(std::string());
        ++wireBVersion_;
    } else if (st == wireb::StitchStep::Failed) {
        stitchEnd(w.stitch.failure());   // (cancel() cannot hide it: the phase is Failed, not active)
        cascade::core::diagLogf("sweep: a stitched sweep ended: %s", w.stitch.failure().c_str());
        ++wireBVersion_;
    }
}

}  // namespace cascade::engine
