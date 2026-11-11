// engine_smeter.cpp - the S-meter in dBm and S units, and its calibration (engine wiring lane 1, group B; docs/multi-radio-records/engine-wiring-1.md).
//
// The block is core/smeter_cal.hpp (pure). This file is what the engine does with it, all on the PASS thread (nothing here touches the DSP thread):
//   * every radio's state carries signalDbm, sUnits and calibrated beside the dBFS figures that were always there (signalDb, sMeter stay on dBFS and the squelch stays on dBFS);
//   * a calibration belongs to a DEVICE (its key, as the gain maps are keyed; the generator and an I/Q file are the keys "siggen" and "file"), one CalTable per device, saved as
//     one text per device in the settings;
//   * CAL_MEASURE averages the selected radio's level for 3 s of passes and stores offset = known dBm - measured dBFS for one band of that device; CAL_CLEAR removes one band or
//     all of them;
//   * the facts fact.smeter.cal.state / .message / .devices say what the last measurement did (LOCAL).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/patch_devices.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

namespace sm = cascade::core::smeter;

std::string Engine::smeterKey(const Receiver& r) const {
    if (r.device_ != nullptr) { return cascade::core::patch::makeDeviceKey(r.sourceKind_, r.deviceArgs_); }
    return r.sourceKind_.empty() ? std::string("siggen") : r.sourceKind_;
}

// The total gain of the radio's tuner as the calibration wants it: the sum of its gain stages, and whether every stage is in decibels (a stage in steps - an Airspy R2's - makes
// the sum a number of no unit, which the calibration then compares for equality only).
void Engine::smeterGain(const Receiver& r, double& gainDb, bool& isDb) const {
    gainDb = 0.0;
    isDb = true;
    for (std::size_t i = 0; i < r.deviceGainRanges_.size(); ++i) {
        if (r.deviceGainRanges_[i].unit != cascade::source::GainUnit::Decibels) { isDb = false; }
        if (i < r.deviceGainsDb_.size()) { gainDb += static_cast<double>(r.deviceGainsDb_[i]); }
    }
}

// The three figures of the state for the selected radio (rx()): dBm, S units and whether a calibration covers the frequency.
void Engine::smeterReading(const Receiver& r, double dbfs, double frequencyHz, double& dbm, double& sUnits, bool& calibrated) const {
    double gain = 0.0;
    bool isDb = true;
    smeterGain(r, gain, isDb);
    (void)isDb;
    const auto it = smeterCal_.find(smeterKey(r));
    sm::CalResult res;
    if (it == smeterCal_.end()) {
        res = sm::applyCal(nullptr, dbfs, gain);
    } else {
        res = it->second.dbm(dbfs, frequencyHz, gain);
    }
    dbm = res.dbm;
    calibrated = res.calibrated;
    sUnits = sm::dbmToS(res.dbm, frequencyHz).units;
}

void Engine::pushAudioEq() {
    for (std::size_t i = 0; i < rx().eqDb_.size(); ++i) { (void)rx().pipeline_.setEqBand(static_cast<int>(i), rx().eqDb_[i]); }
    (void)rx().pipeline_.setAudioFilters(rx().audioHpHz_, rx().audioLpHz_);
}

void Engine::pushManualNotches() {
    for (std::size_t i = 0; i < rx().manualNotch_.size(); ++i) {
        const Receiver::ManualNotchSlot& ns = rx().manualNotch_[i];
        (void)rx().pipeline_.setManualNotch(static_cast<int>(i), ns.hz, ns.widthHz, ns.on);
    }
}

std::int32_t Engine::applySmeterOp(const FoxCommand& c, FoxCommandResult& res, std::string& why) {
    Receiver& r = rx();
    const std::string key = smeterKey(r);
    const double hz = std::max(0.0, currentAbsoluteHz());
    if (c.op == FOXAPP_OP_CAL_CLEAR) {
        const auto it = smeterCal_.find(key);
        if (it == smeterCal_.end() || it->second.size() == 0) {
            why = "this radio's device has no calibration";
            return FOXAPI_NOT_FOUND;
        }
        if (c.ival[0] == 1) {
            smeterCal_.erase(it);
        } else {
            const double at = (c.num[0] > 0.0) ? c.num[0] : hz;
            if (!it->second.clearAt(at)) {
                why = "no calibration covers that frequency";
                return FOXAPI_NOT_FOUND;
            }
            if (it->second.size() == 0) { smeterCal_.erase(it); }
        }
        ++smeterVersion_;
        res.applied[0] = static_cast<double>(smeterCal_.count(key) != 0 ? smeterCal_[key].size() : 0);
        return FOXAPI_OK;
    }
    // CAL_MEASURE
    if (smeterMeasure_.active) {
        why = "a calibration is being measured already";
        return FOXAPI_BUSY;
    }
    if (!std::isfinite(c.num[0]) || c.num[0] < -150.0 || c.num[0] > 30.0) {
        why = "the known level must be between -150 and +30 dBm";
        return FOXAPI_OUT_OF_RANGE;
    }
    double lo = 0.0, hi = 30.0e6;
    if (c.ival[0] == 1) {
        lo = c.num[1];
        hi = c.num[2];
        if (!(lo >= 0.0) || !(hi > lo) || hi > 6.0e9 || !std::isfinite(hi)) {
            why = "the band must run from 0 Hz up to 6 GHz, lowest first";
            return FOXAPI_OUT_OF_RANGE;
        }
    } else if (hz >= sm::kHfVhfBoundaryHz) {
        lo = sm::kHfVhfBoundaryHz;
        hi = 6.0e9;
    }
    if (!r.pipeline_.running()) {
        why = "the radio is not running: there is no level to measure";
        return FOXAPI_BUSY;
    }
    smeterMeasure_ = SmeterMeasure{};
    smeterMeasure_.active = true;
    smeterMeasure_.slot = scopedSlot();
    smeterMeasure_.key = key;
    smeterMeasure_.knownDbm = c.num[0];
    smeterMeasure_.lo = lo;
    smeterMeasure_.hi = hi;
    smeterMeasure_.startS = clockS();
    smeterState_ = "measuring";
    smeterMessage_ = "measuring for 3 seconds";
    ++smeterVersion_;
    res.applied[0] = lo;
    res.applied[1] = hi;
    return FOXAPI_OK;
}

// Once a pass: sample the measured radio's level; after kSmeterMeasureS seconds store the offset.
void Engine::pollSmeterCal() {
    if (!smeterMeasure_.active) { return; }
    SmeterMeasure& m = smeterMeasure_;
    const auto fail = [this, &m](const std::string& why) {
        m.active = false;
        smeterState_ = "failed";
        smeterMessage_ = why;
        ++smeterVersion_;
    };
    if (m.slot >= kMaxReceivers || !receivers_[m.slot] || receivers_[m.slot]->removing_) { return fail("the radio was removed"); }
    Receiver& r = *receivers_[m.slot];
    if (!r.pipeline_.running()) { return fail("the radio stopped"); }
    if (smeterKey(r) != m.key) { return fail("the radio's source changed"); }
    double gain = 0.0;
    bool isDb = true;
    smeterGain(r, gain, isDb);
    if (m.n == 0) {
        m.gainDb = gain;
        m.gainIsDb = isDb;
    } else if (gain != m.gainDb) {
        return fail("the gain was changed during the measurement");
    }
    m.sum += static_cast<double>(r.pipeline_.signalPowerDb());
    ++m.n;
    if (clockS() - m.startS < kSmeterMeasureS) { return; }
    const double avg = m.sum / static_cast<double>(m.n);
    double offset = 0.0;
    if (!sm::offsetFromMeasurement(m.knownDbm, avg, offset)) { return fail("no signal was measured (the level is at the meter's floor)"); }
    sm::CalEntry e;
    e.loHz = m.lo;
    e.hiHz = m.hi;
    e.offsetDb = offset;
    e.gainDb = m.gainDb;
    e.gainIsDb = m.gainIsDb;
    sm::CalTable& table = smeterCal_[m.key];
    if (!table.set(e)) {
        if (table.size() == 0) { smeterCal_.erase(m.key); }
        return fail("the calibration could not be stored (the table of this device is full, or the offset is out of range)");
    }
    m.active = false;
    smeterState_ = "done";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "offset %.1f dB from %d samples", offset, m.n);
    smeterMessage_ = buf;
    ++smeterVersion_;
}

}  // namespace cascade::engine
