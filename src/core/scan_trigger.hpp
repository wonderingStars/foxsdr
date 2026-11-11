// A scan TRIGGER level that is separate from the demodulator's squelch, with a noise floor that follows the band as the scan moves.
//
// WHY. The squelch decides whether the AUDIO opens; it is set for listening (the demodulator's own level, in dB or by the automatic floor + 8 dB of
// core/noise_floor_tracker.hpp). A scan wants its own, usually different, rule for "something is here": a fixed level in dB, or a margin above the
// noise floor of THIS frequency, because the floor of a band is not the floor of the next band (a scan across 118-137 MHz and then 144-146 MHz meets
// different noise). This model answers only that question, one reading at a time; it owns no thread, no clock and no radio. The squelch is not read
// and not changed.
//
// THE RULE. Each scan slot (a lattice point or a list entry, by index) has its own floor estimate in dB. A reading is "present" when
//     Fixed    : level >= fixedDb                      (trigger),   released when level < fixedDb - hysteresisDb
//     Relative : level >= floor[slot] + marginDb       (trigger),   released when level < floor[slot] + marginDb - hysteresisDb
// The floor of a slot follows the readings DOWN at once and UP slowly (riseDbPerReading per reading, default 0.1 dB), and is NOT updated while the slot
// is triggered, so a carrier never teaches the floor (the reason the floor of core/noise_floor_tracker.hpp is a minimum). The first reading of a slot
// seeds its floor; a slot with no reading yet never triggers in Relative mode on that first reading (it is only the seed).
//
// "no data": NaN, +-inf and readings below kNoDataDb (-200 dB) are ignored: they neither trigger, release, nor teach the floor; update() returns the
// previous state of the slot.
//
// THREADS: one thread (the control thread that runs the scanner tick). Not thread safe.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::core::tuning {

inline constexpr double kNoDataDb = -200.0;

enum class TriggerMode { Fixed = 0, Relative = 1 };

struct ScanTriggerConfig {
    TriggerMode mode = TriggerMode::Relative;
    double fixedDb = -70.0;          // Fixed: absolute level
    double marginDb = 8.0;           // Relative: dB above the slot's floor (the squelch's automatic rule also uses 8)
    double hysteresisDb = 2.0;       // release this far below the trigger
    double riseDbPerReading = 0.1;   // how fast the floor may rise
};

inline bool validTriggerConfig(const ScanTriggerConfig& c) {
    return std::isfinite(c.fixedDb) && c.fixedDb >= -160.0 && c.fixedDb <= 20.0 && std::isfinite(c.marginDb) && c.marginDb >= 0.0 && c.marginDb <= 60.0 &&
           std::isfinite(c.hysteresisDb) && c.hysteresisDb >= 0.0 && c.hysteresisDb <= 30.0 && std::isfinite(c.riseDbPerReading) && c.riseDbPerReading >= 0.0 &&
           c.riseDbPerReading <= 10.0;
}

class ScanTrigger {
public:
    static constexpr std::size_t kMaxSlots = 200000;

    // false (nothing changed) for an invalid config. A change of config keeps the floors.
    bool setConfig(const ScanTriggerConfig& c) {
        if (!validTriggerConfig(c)) { return false; }
        cfg_ = c;
        return true;
    }
    const ScanTriggerConfig& config() const { return cfg_; }

    // Forget every floor and every state (a new scan plan: the slots mean other frequencies).
    void reset() { slots_.clear(); }

    // One reading of slot `slot` in dB. Returns whether the slot is triggered after this reading. A slot index at or beyond kMaxSlots is refused (false).
    bool update(std::size_t slot, double levelDb) {
        if (slot >= kMaxSlots) { return false; }
        if (slot >= slots_.size()) { slots_.resize(slot + 1); }
        Slot& s = slots_[slot];
        if (!std::isfinite(levelDb) || levelDb < kNoDataDb) { return s.triggered; }
        if (!s.seeded) {
            s.seeded = true;
            s.floorDb = levelDb;
            if (cfg_.mode == TriggerMode::Fixed) { s.triggered = levelDb >= cfg_.fixedDb; }
            return s.triggered;  // Relative: the first reading is only the seed
        }
        const double trig = triggerDb(s);
        if (s.triggered) {
            if (levelDb < trig - cfg_.hysteresisDb) { s.triggered = false; }
        } else if (levelDb >= trig) {
            s.triggered = true;
        }
        if (!s.triggered) {
            if (levelDb < s.floorDb) { s.floorDb = levelDb; }
            else { s.floorDb = std::min(levelDb, s.floorDb + cfg_.riseDbPerReading); }
        }
        return s.triggered;
    }

    bool triggered(std::size_t slot) const { return slot < slots_.size() && slots_[slot].triggered; }
    // The floor of a slot, or kNoDataDb when it has none.
    double floorDb(std::size_t slot) const { return slot < slots_.size() && slots_[slot].seeded ? slots_[slot].floorDb : kNoDataDb; }
    // The level at which the slot triggers now (Fixed: fixedDb; Relative: floor + margin; no floor yet: +infinity).
    double triggerLevelDb(std::size_t slot) const {
        if (cfg_.mode == TriggerMode::Fixed) { return cfg_.fixedDb; }
        if (slot >= slots_.size() || !slots_[slot].seeded) { return INFINITY; }
        return slots_[slot].floorDb + cfg_.marginDb;
    }
    std::size_t slotCount() const { return slots_.size(); }

    // The config as versioned JSON (the floors are measurements and are not saved).
    nlohmann::json configToJson() const {
        return {{"version", 1}, {"mode", cfg_.mode == TriggerMode::Fixed ? "fixed" : "relative"}, {"fixedDb", cfg_.fixedDb}, {"marginDb", cfg_.marginDb},
                {"hysteresisDb", cfg_.hysteresisDb}, {"riseDbPerReading", cfg_.riseDbPerReading}};
    }
    // Strict: another version, an unknown mode, a missing or non-number field or an out-of-range value changes nothing and returns false.
    bool configFromJson(const nlohmann::json& j, std::string* err = nullptr) {
        auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
        if (!j.is_object()) { return fail("not an object"); }
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail("unknown version"); }
        ScanTriggerConfig c;
        if (!j.contains("mode") || !j["mode"].is_string()) { return fail("mode missing"); }
        const std::string m = j["mode"].get<std::string>();
        if (m == "fixed") { c.mode = TriggerMode::Fixed; } else if (m == "relative") { c.mode = TriggerMode::Relative; } else { return fail("unknown mode"); }
        const char* names[] = {"fixedDb", "marginDb", "hysteresisDb", "riseDbPerReading"};
        double* dst[] = {&c.fixedDb, &c.marginDb, &c.hysteresisDb, &c.riseDbPerReading};
        for (int i = 0; i < 4; ++i) {
            if (!j.contains(names[i]) || !j[names[i]].is_number()) { return fail("a field is missing or not a number"); }
            *dst[i] = j[names[i]].get<double>();
        }
        if (!validTriggerConfig(c)) { return fail("a value is out of range"); }
        cfg_ = c;
        return true;
    }

private:
    struct Slot { bool seeded = false; bool triggered = false; double floorDb = kNoDataDb; };
    double triggerDb(const Slot& s) const { return cfg_.mode == TriggerMode::Fixed ? cfg_.fixedDb : s.floorDb + cfg_.marginDb; }

    ScanTriggerConfig cfg_{};
    std::vector<Slot> slots_;
};

}  // namespace cascade::core::tuning
