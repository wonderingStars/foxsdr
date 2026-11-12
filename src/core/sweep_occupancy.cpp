// sweep_occupancy.cpp - see sweep_occupancy.hpp for the rules.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sweep_occupancy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cascade::core {

double ChannelPlan::channelStopHz(std::size_t i) const {
    return std::min(stopHz, startHz + stepHz * static_cast<double>(i + 1));
}

bool makeChannelPlan(double startHz, double stopHz, double stepHz, ChannelPlan& out, std::string* why) {
    auto fail = [&](const char* w) { if (why != nullptr) { *why = w; } return false; };
    if (!std::isfinite(startHz) || !std::isfinite(stopHz) || !std::isfinite(stepHz)) { return fail("plan is not a number"); }
    if (startHz < 0.0 || stopHz > kSweepMaxFrequencyHz) { return fail("plan frequency out of range"); }
    if (!(stopHz > startHz)) { return fail("stop must be above start"); }
    if (stepHz < 1.0) { return fail("step below 1 Hz"); }
    const double n = std::ceil((stopHz - startHz) / stepHz - 1e-9);
    if (n < 1.0 || n > static_cast<double>(kOccMaxChannels)) { return fail("too many channels"); }
    out.startHz = startHz;
    out.stopHz = stopHz;
    out.stepHz = stepHz;
    out.count = static_cast<std::size_t>(n);
    return true;
}

bool OccupancyModel::configure(const OccupancyConfig& cfg, std::string* why) {
    auto fail = [&](const char* w) { if (why != nullptr) { *why = w; } return false; };
    if (cfg.plan.count < 1 || cfg.plan.count > kOccMaxChannels) { return fail("no channel plan"); }
    if (!std::isfinite(cfg.marginDb) || cfg.marginDb < 0.0 || cfg.marginDb > 200.0) { return fail("margin out of range"); }
    if (!(cfg.floorPercentile >= 0.0 && cfg.floorPercentile <= 0.5)) { return fail("floor percentile out of range"); }
    if (cfg.fixedFloor && (!std::isfinite(cfg.fixedFloorDb) || cfg.fixedFloorDb < kSweepMinLevelDb || cfg.fixedFloorDb > kSweepMaxLevelDb)) {
        return fail("fixed floor out of range");
    }
    if (cfg.minChannelsForPercentile < 1) { return fail("minimum channels must be at least 1"); }
    if (!(cfg.maxGapS > 0.0) || !std::isfinite(cfg.maxGapS) || !(cfg.nominalDwellS > 0.0) || !std::isfinite(cfg.nominalDwellS)) {
        return fail("gap or dwell not positive");
    }
    if (cfg.utcOffsetMinutes < -840 || cfg.utcOffsetMinutes > 840) { return fail("UTC offset out of range"); }
    cfg_ = cfg;
    sweeps_ = 0;
    obs_.assign(cfg.plan.count, 0.0);
    occ_.assign(cfg.plan.count, 0.0);
    hobs_.assign(cfg.plan.count * 24, 0.0);
    hocc_.assign(cfg.plan.count * 24, 0.0);
    pending_.assign(cfg.plan.count, 0);
    havePending_ = false;
    lastDt_ = 0.0;
    configured_ = true;
    return true;
}

int OccupancyModel::hourOf(double timeS) const {
    const double local = timeS + 60.0 * cfg_.utcOffsetMinutes;
    double s = std::fmod(local, 86400.0);
    if (s < 0.0) { s += 86400.0; }
    int h = static_cast<int>(s / 3600.0);
    if (h > 23) { h = 23; }
    return h;
}

void OccupancyModel::credit(double dt) {
    const int h = hourOf(pendingTime_);
    for (std::size_t c = 0; c < pending_.size(); ++c) {
        if (pending_[c] == 0) { continue; }
        obs_[c] += dt;
        hobs_[c * 24 + static_cast<std::size_t>(h)] += dt;
        if (pending_[c] == 2) {
            occ_[c] += dt;
            hocc_[c * 24 + static_cast<std::size_t>(h)] += dt;
        }
    }
}

bool OccupancyModel::addSweep(const SweepLine& line, std::string* why) {
    auto fail = [&](const char* w) { if (why != nullptr) { *why = w; } return false; };
    if (!configured_) { return fail("not configured"); }
    if (!sweepLineProblem(line).empty()) { return fail("line refused"); }
    if (havePending_ && !(line.timeS > pendingTime_)) { return fail("time does not increase"); }

    const ChannelPlan& plan = cfg_.plan;
    const std::size_t n = plan.count;
    std::vector<float> level(n, std::nanf(""));
    for (std::size_t i = 0; i < line.db.size(); ++i) {
        const float v = line.db[i];
        if (std::isnan(v)) { continue; }
        const double f = sweepPointHz(line, i);
        if (f < plan.startHz || f >= plan.stopHz) { continue; }
        std::size_t c = static_cast<std::size_t>(std::floor((f - plan.startHz) / plan.stepHz));
        if (c >= n) { c = n - 1; }
        if (std::isnan(level[c]) || v > level[c]) { level[c] = v; }
    }
    double floorDb = cfg_.fixedFloorDb;
    if (!cfg_.fixedFloor) {
        levelScratch_.clear();
        for (float v : level) { if (!std::isnan(v)) { levelScratch_.push_back(v); } }
        if (levelScratch_.size() < cfg_.minChannelsForPercentile) { return fail("too few observed channels for a floor estimate"); }
        std::sort(levelScratch_.begin(), levelScratch_.end());
        floorDb = levelScratch_[static_cast<std::size_t>(std::floor(cfg_.floorPercentile * static_cast<double>(levelScratch_.size() - 1)))];
    }
    const double threshold = floorDb + cfg_.marginDb;

    if (havePending_) {
        lastDt_ = std::min(line.timeS - pendingTime_, cfg_.maxGapS);
        credit(lastDt_);
    }
    for (std::size_t c = 0; c < n; ++c) {
        pending_[c] = std::isnan(level[c]) ? 0 : (static_cast<double>(level[c]) > threshold ? 2 : 1);
    }
    pendingTime_ = line.timeS;
    havePending_ = true;
    lastThreshold_ = threshold;
    ++sweeps_;
    return true;
}

void OccupancyModel::finish() {
    if (!havePending_) { return; }
    credit(lastDt_ > 0.0 ? lastDt_ : cfg_.nominalDwellS);
    havePending_ = false;
    std::fill(pending_.begin(), pending_.end(), std::uint8_t{0});
}

double OccupancyModel::observedS(std::size_t ch) const { return ch < obs_.size() ? obs_[ch] : 0.0; }
double OccupancyModel::occupiedS(std::size_t ch) const { return ch < occ_.size() ? occ_[ch] : 0.0; }

double OccupancyModel::percent(std::size_t ch) const {
    if (ch >= obs_.size() || !(obs_[ch] > 0.0)) { return std::nan(""); }
    return 100.0 * occ_[ch] / obs_[ch];
}

double OccupancyModel::hourPercent(int hour) const {
    if (hour < 0 || hour > 23) { return std::nan(""); }
    double o = 0, c = 0;
    for (std::size_t ch = 0; ch < obs_.size(); ++ch) {
        o += hobs_[ch * 24 + static_cast<std::size_t>(hour)];
        c += hocc_[ch * 24 + static_cast<std::size_t>(hour)];
    }
    return o > 0.0 ? 100.0 * c / o : std::nan("");
}

double OccupancyModel::hourPercent(std::size_t ch, int hour) const {
    if (ch >= obs_.size() || hour < 0 || hour > 23) { return std::nan(""); }
    const double o = hobs_[ch * 24 + static_cast<std::size_t>(hour)];
    return o > 0.0 ? 100.0 * hocc_[ch * 24 + static_cast<std::size_t>(hour)] / o : std::nan("");
}

namespace {
std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}
std::string pct(double obs, double occ) { return obs > 0.0 ? num(100.0 * occ / obs) : std::string(); }
}  // namespace

std::string OccupancyModel::channelsCsv() const {
    std::string s = "#foxsdr-occupancy,channels,1\nchannel,start_hz,stop_hz,observed_s,occupied_s,percent\n";
    for (std::size_t c = 0; c < obs_.size(); ++c) {
        s += std::to_string(c) + "," + num(cfg_.plan.channelStartHz(c)) + "," + num(cfg_.plan.channelStopHz(c)) + "," +
             num(obs_[c]) + "," + num(occ_[c]) + "," + pct(obs_[c], occ_[c]) + "\n";
    }
    return s;
}

std::string OccupancyModel::hoursCsv() const {
    std::string s = "#foxsdr-occupancy,hours,1\nhour,observed_s,occupied_s,percent\n";
    for (int h = 0; h < 24; ++h) {
        double o = 0, c = 0;
        for (std::size_t ch = 0; ch < obs_.size(); ++ch) {
            o += hobs_[ch * 24 + static_cast<std::size_t>(h)];
            c += hocc_[ch * 24 + static_cast<std::size_t>(h)];
        }
        s += std::to_string(h) + "," + num(o) + "," + num(c) + "," + pct(o, c) + "\n";
    }
    return s;
}

std::string OccupancyModel::channelHoursCsv() const {
    std::string s = "#foxsdr-occupancy,channel-hours,1\nchannel,hour,observed_s,occupied_s,percent\n";
    for (std::size_t ch = 0; ch < obs_.size(); ++ch) {
        for (int h = 0; h < 24; ++h) {
            const double o = hobs_[ch * 24 + static_cast<std::size_t>(h)];
            if (!(o > 0.0)) { continue; }
            const double c = hocc_[ch * 24 + static_cast<std::size_t>(h)];
            s += std::to_string(ch) + "," + std::to_string(h) + "," + num(o) + "," + num(c) + "," + pct(o, c) + "\n";
        }
    }
    return s;
}

}  // namespace cascade::core
