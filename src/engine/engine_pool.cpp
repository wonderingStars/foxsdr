// engine_pool.cpp - TUNER POOLING (engine serial lane A, row R19): FOXAPP_OP_CHANNEL_CLAIM / CHANNEL_RELEASE. The choice is core/tuner_pool.hpp's (pure); this builds its candidate list from the open
// radios, acts on the answer under the chosen radio's scope, and keeps the claim (Receiver::claimed_, fact.radio.<n>.claimed) until it is released.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/receiver_tables.hpp"
#include "core/tuner_pool.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

namespace {
FoxCommandResult commandResultFor(const FoxCommand& c) {
    FoxCommandResult r;
    std::memset(&r, 0, sizeof(r));
    r.structSize = static_cast<std::uint32_t>(sizeof(FoxCommandResult));
    r.status = FOXAPI_OK;
    r.op = c.op;
    return r;
}
FoxCommandResult refused(FoxCommandResult res, std::int32_t status, const std::string& why) {
    res.status = status;
    res.flags |= FOXAPI_RESULT_REFUSED;
    std::snprintf(res.message, sizeof(res.message), "%s", why.c_str());
    return res;
}
}  // namespace

FoxCommandResult Engine::claimChannel(const FoxCommand& c) {
    FoxCommandResult res = commandResultFor(c);
    const double hz = c.num[0];
    if (!(hz > 0.0) || hz > 1e12) { return refused(res, FOXAPI_OUT_OF_RANGE, "a claim names a frequency in Hz"); }
    if (c.ival[0] < 0 || c.ival[0] > 8) { return refused(res, FOXAPI_OUT_OF_RANGE, "no such mode"); }
    if (!(c.num[1] >= 0.0) || c.num[1] > 1e9) { return refused(res, FOXAPI_OUT_OF_RANGE, "the bandwidth is 0 (leave it) or a width in Hz"); }
    std::vector<cascade::core::pool::Candidate> cands;
    int busy = 0, open = 0;
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        if (!receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        ++open;
        SlotScope scope(this, slot);
        cascade::core::pool::Candidate k;
        k.number = static_cast<int>(slot) + 1;
        const bool lent = slot == 0 && patchMainKeep_.valid;
        k.free = !lent && !receivers_[slot]->claimed_ && !receivers_[slot]->tuneLocked_ && !rx().iqRecorder_.recording() && !rx().audioRecorder_.recording() &&
                 !(scanner_.active() && scannerSlot_ == slot) && rx().pipeline_.running() && !rx().deviceOpenPending_;
        if (!k.free) { ++busy; }
        k.centreHz = rx().pipeline_.activeSource().centerFrequencyHz();
        k.rateHz = rx().pipeline_.inputRateHz();
        k.channelHz = c.num[1] > 0.0 ? c.num[1] : rx().vfoBandwidthHz_;
        double lo = 0.0, hi = 0.0;
        if (rx().device_ != nullptr && rx().device_->frequencyRangeHz(lo, hi)) {
            k.rangeLoHz = lo;
            k.rangeHiHz = hi;
        }
        cands.push_back(k);
    }
    const cascade::core::pool::Pick pick = cascade::core::pool::choose(cands, hz);
    if (pick.how == cascade::core::pool::Verdict::None) {
        return refused(res, FOXAPI_LIMIT, open == busy ? "no radio is free: every open radio is claimed, locked, recording, scanning or not running" : "no free radio can tune there");
    }
    const std::size_t slot = static_cast<std::size_t>(pick.number) - 1u;
    {
        SlotScope scope(this, slot);
        if (c.ival[0] > 0) { setModeIndex(static_cast<int>(c.ival[0]) - 1); }
        if (pick.how == cascade::core::pool::Verdict::ByVfo) {
            rx().pipeline_.setVfoOffsetHz(hz - rx().pipeline_.activeSource().centerFrequencyHz());
        } else {
            rx().pipeline_.setVfoOffsetHz(0.0);
            tuneAbsoluteHz(hz);
        }
        if (c.num[1] > 0.0) {
            const double hiBw = cascade::gui::kVfoBwMaxChanFrac * rx().pipeline_.channelRateHz();
            rx().vfoBandwidthHz_ = std::max(cascade::gui::kVfoBwMinHz, std::min(c.num[1], hiBw));
            applyChannelBandwidth();
            rx().bandwidthIndex_ = cascade::gui::bandwidthStepIndex(rx().vfoBandwidthHz_);
        }
        rx().claimed_ = true;
    }
    ++poolVersion_;
    res.applied[0] = static_cast<double>(pick.number);
    res.applied[1] = pick.how == cascade::core::pool::Verdict::ByVfo ? 1.0 : 2.0;
    return res;
}

FoxCommandResult Engine::releaseChannel(const FoxCommand& c) {
    FoxCommandResult res = commandResultFor(c);
    const long long n = static_cast<long long>(c.ival[0]);
    if (n < 1 || n > static_cast<long long>(kMaxReceivers) || !receiverLive(static_cast<std::uint32_t>(n))) { return refused(res, FOXAPI_NO_DEVICE, "no radio has that number"); }
    Receiver& r = *receivers_[static_cast<std::size_t>(n) - 1u];
    if (!r.claimed_) { return refused(res, FOXAPI_NO_CHANGE, "that radio is not claimed"); }
    r.claimed_ = false;
    ++poolVersion_;
    res.applied[0] = static_cast<double>(n);
    return res;
}

}  // namespace cascade::engine
