// wire_b_stitch.cpp - the stitched sweep loop's state machine (wire_b_stitch.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_b_stitch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cascade::engine::wireb {

namespace {
std::string mhz(double hz) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.3f", hz / 1.0e6);
    return b;
}
}  // namespace

bool StitchRun::begin(const StitchSettings& s, double rateHz, double nowS, std::string& why) {
    (void)nowS;
    phase_ = Phase::Idle;
    if (!std::isfinite(s.startHz) || !std::isfinite(s.stopHz) || !(s.startHz >= 0.0) || !(s.stopHz > s.startHz)) { why = "the stop frequency must be above the start frequency"; return false; }
    if (!(rateHz > 0.0) || !std::isfinite(rateHz)) { why = "the radio has no sample rate yet: start it first"; return false; }
    if (!(s.stepFraction >= 0.3 && s.stepFraction <= 0.7)) { why = "the step is 0.3 to 0.7 of the sample rate"; return false; }
    if (!(s.trimFraction >= 0.0 && s.trimFraction <= 0.4)) { why = "the trim is 0 to 0.4"; return false; }
    if (s.settleMs < 0 || s.settleMs > 5000) { why = "the settle time is 0 to 5000 ms"; return false; }
    if (s.frames < 1 || s.frames > 64) { why = "the frames per step are 1 to 64"; return false; }
    if (s.timeoutMs < 500 || s.timeoutMs > 60000) { why = "the time-out is 500 to 60000 ms"; return false; }
    if (s.passes < 0 || s.passes > 1000000) { why = "the passes are 0 (until closed) to 1000000"; return false; }
    const double half = (0.5 - s.trimFraction) * rateHz;   // the kept half width of one capture
    const double span = s.stopHz - s.startHz;
    std::vector<double> c;
    if (span <= 2.0 * half) {
        c.push_back(0.5 * (s.startHz + s.stopHz));
    } else {
        const double step = s.stepFraction * rateHz;
        const double steps = std::ceil((span - 2.0 * half) / step);
        if (steps + 1.0 > static_cast<double>(kStitchMaxSteps)) { why = "that range needs more than 512 centres at this sample rate: narrow it"; return false; }
        const std::size_t n = static_cast<std::size_t>(steps) + 1;
        const double eff = (span - 2.0 * half) / static_cast<double>(n - 1);   // (never above the step: neighbours always overlap at least as planned)
        for (std::size_t k = 0; k < n; ++k) { c.push_back(s.startHz + half + static_cast<double>(k) * eff); }
    }
    s_ = s;
    rateHz_ = rateHz;
    centres_ = std::move(c);
    k_ = 0;
    passesDone_ = 0;
    caps_.clear();
    power_.clear();
    bins_ = 0;
    got_ = 0;
    haveSeq_ = false;
    haveLine_ = false;
    failure_.clear();
    note_.clear();
    alignedSeams_ = seams_ = 0;
    lastBinHz_ = 0.0;
    phase_ = Phase::NeedRetune;
    return true;
}

void StitchRun::fail(const std::string& sentence) {
    failure_ = sentence;
    phase_ = Phase::Failed;
}

bool StitchRun::takeLine(cascade::core::SweepLine& out) {
    if (!haveLine_) { return false; }
    out = std::move(line_);
    haveLine_ = false;
    return true;
}

StitchStep StitchRun::advance(double nowS, const StitchObservation& obs) {
    switch (phase_) {
        case Phase::Idle:
        case Phase::Done:
        case Phase::Failed:
            return StitchStep::Nothing;
        case Phase::Finishing:
            phase_ = Phase::Done;
            return StitchStep::Finished;
        case Phase::NeedRetune:
            phase_ = Phase::Settle;
            settleEndS_ = nowS + static_cast<double>(s_.settleMs) / 1000.0;
            deadlineS_ = settleEndS_ + static_cast<double>(s_.timeoutMs) / 1000.0;
            note_ = "centre " + std::to_string(k_ + 1) + " of " + std::to_string(centres_.size()) + ": " + mhz(centres_[k_]) + " MHz";
            return StitchStep::Retune;
        case Phase::Settle: {
            if (obs.rateHz > 0.0 && std::fabs(obs.rateHz - rateHz_) > 0.005 * rateHz_) { fail("the radio's sample rate changed during the sweep"); return StitchStep::Failed; }
            if (nowS < settleEndS_) { return StitchStep::Nothing; }
            if (obs.haveFrame && std::fabs(obs.centreHz - centres_[k_]) <= 0.05 * rateHz_) {
                phase_ = Phase::Collect;
                lastSeq_ = obs.seq;   // only frames AFTER this one count
                haveSeq_ = true;
                got_ = 0;
                bins_ = 0;
                power_.clear();
                deadlineS_ = nowS + static_cast<double>(s_.timeoutMs) / 1000.0;
                return StitchStep::Nothing;
            }
            if (nowS > deadlineS_) {
                const bool there = std::fabs(obs.centreHz - centres_[k_]) <= 0.05 * rateHz_;
                fail(there ? std::string("no spectrum frames arrived from the radio") : "the radio did not retune to " + mhz(centres_[k_]) + " MHz (is it playing a file, or locked?)");
                return StitchStep::Failed;
            }
            return StitchStep::Nothing;
        }
        case Phase::Collect: {
            if (!obs.haveFrame || obs.db == nullptr || !haveSeq_ || obs.seq <= lastSeq_) {
                if (nowS > deadlineS_) { fail("no spectrum frames arrived from the radio"); return StitchStep::Failed; }
                return StitchStep::Nothing;
            }
            lastSeq_ = obs.seq;
            deadlineS_ = nowS + static_cast<double>(s_.timeoutMs) / 1000.0;
            if (obs.rateHz > 0.0 && std::fabs(obs.rateHz - rateHz_) > 0.005 * rateHz_) { fail("the radio's sample rate changed during the sweep"); return StitchStep::Failed; }
            if (std::fabs(obs.centreHz - centres_[k_]) > 0.05 * rateHz_) { fail("the radio's centre was changed during the sweep"); return StitchStep::Failed; }
            if (obs.db->empty()) { return StitchStep::Nothing; }
            if (bins_ == 0) { bins_ = obs.db->size(); power_.assign(bins_, 0.0); }
            else if (obs.db->size() != bins_) { fail("the spectrum size changed during the sweep"); return StitchStep::Failed; }
            for (std::size_t i = 0; i < bins_; ++i) {
                const float v = (*obs.db)[i];
                if (std::isfinite(v)) { power_[i] += std::pow(10.0, static_cast<double>(v) / 10.0); }
            }
            capCentre_ = obs.centreHz;
            ++got_;
            if (got_ < s_.frames) { return StitchStep::Nothing; }
            cascade::core::SpectrumCapture cap;
            cap.centerHz = capCentre_;
            cap.rateHz = rateHz_;
            cap.timeS = nowS;
            cap.db.resize(bins_);
            for (std::size_t i = 0; i < bins_; ++i) {
                const double m = power_[i] / static_cast<double>(got_);
                cap.db[i] = m > 0.0 ? static_cast<float>(10.0 * std::log10(m)) : std::nanf("");
            }
            caps_.push_back(std::move(cap));
            ++k_;
            if (k_ < centres_.size()) { phase_ = Phase::NeedRetune; return StitchStep::Nothing; }
            // the pass is complete: one wide line
            cascade::core::StitchConfig cfg;
            cfg.trimFraction = s_.trimFraction;
            cascade::core::StitchResult res = cascade::core::stitchPass(caps_, cfg);
            caps_.clear();
            k_ = 0;
            if (!res.ok) { fail("the pass could not be stitched: " + res.error); return StitchStep::Failed; }
            ++passesDone_;
            seams_ = res.seams.size();
            alignedSeams_ = 0;
            for (const cascade::core::StitchSeam& sm : res.seams) { if (sm.aligned) { ++alignedSeams_; } }
            lastBinHz_ = res.binHz;
            line_ = std::move(res.line);
            haveLine_ = true;
            phase_ = (s_.passes != 0 && passesDone_ >= static_cast<std::uint64_t>(s_.passes)) ? Phase::Finishing : Phase::NeedRetune;
            return StitchStep::LineReady;
        }
    }
    return StitchStep::Nothing;
}

}  // namespace cascade::engine::wireb
