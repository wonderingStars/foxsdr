// Operating aids as pure logic: frequency lock and tuning-step lock, band-stacking registers, VFO A/B with swap, copy and split, and a per-mode memory of
// bandwidth, step and AGC. No engine, no screen, no files: the wiring lane connects them (docs/feature-records/recording-models.md, Part 7).
//
// WHAT THE REFERENCE PROGRAMS DO is taken from the owner's gap list (FEATURE-GAPS.md, "Tuning and operating aids"): frequency lock (SDR# plugin, Malahit,
// Thetis VFO lock), tuning-step lock (SDRuno), band-stacking registers (HDSDR, SDR Console), VFO A/B and split (HDSDR, transceiver programs). No program
// source was read; the behaviours below are written from those one-line descriptions and from ordinary operating practice, and every default is marked
// as a default the owner can change.
//
// THREADS: every class is for ONE thread (the control thread that owns the settings); none is thread safe. Nothing allocates after construction except
// the list set-ups and the JSON conversions.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::core::aids {

constexpr double kMaxFrequencyHz = 1e13;    // far above any receiver; rejects garbage, not hardware limits (the radio's own range is the engine's job)

inline bool validFrequency(double hz) { return std::isfinite(hz) && hz >= 0.0 && hz <= kMaxFrequencyHz; }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 1. Frequency lock and tuning-step lock.
// ---------------------------------------------------------------------------------------------------------------------------------------------
// WHERE a tuning request came from. The lock blocks the ones a hand can do by accident; the others are explicit acts (a bookmark click, a band button,
// a running scanner, a remote client) and are allowed unless the policy says otherwise.
enum class TuneSource : int { Wheel = 0, Click = 1, Drag = 2, KeyStep = 3, Knob = 4, TypedEntry = 5, Bookmark = 6, Band = 7, Scanner = 8, Remote = 9, Program = 10 };
inline constexpr int kTuneSourceCount = 11;

struct LockPolicy {
    // DEFAULT (owner can change): the lock stops the controls a hand brushes against (wheel, click on the spectrum, drag, step keys, knobs) and typed entry;
    // bookmarks, band buttons, the scanner, remote clients and program calls are explicit and pass.
    std::array<bool, kTuneSourceCount> blocked = {true, true, true, true, true, true, false, false, false, false, false};
};

struct TuneDecision {
    bool allowed = true;
    double hz = 0.0;           // the frequency to use: the requested one when allowed, else the current one
    std::string reason;        // empty when allowed; else a short text for the status line ("frequency locked")
};

class TuneLock {
public:
    void setFrequencyLocked(bool on) { freqLocked_ = on; }
    bool frequencyLocked() const { return freqLocked_; }
    void setStepLocked(bool on) { stepLocked_ = on; }
    bool stepLocked() const { return stepLocked_; }
    void setPolicy(const LockPolicy& p) { policy_ = p; }
    const LockPolicy& policy() const { return policy_; }

    // `current` is where the radio is now. Non-finite or out-of-range requests are always refused (lock or no lock).
    TuneDecision requestFrequency(double current, double requested, TuneSource src) const {
        TuneDecision d;
        d.hz = current;
        if (!validFrequency(requested)) { d.allowed = false; d.reason = "not a valid frequency"; return d; }
        const int i = static_cast<int>(src);
        if (freqLocked_ && i >= 0 && i < kTuneSourceCount && policy_.blocked[static_cast<std::size_t>(i)]) {
            d.allowed = false; d.reason = "frequency locked";
            return d;
        }
        d.hz = requested;
        return d;
    }

    // The tuning step (Hz). When locked the current step stays. A step must be finite and in [1 Hz, 10 MHz].
    struct StepDecision { bool allowed = true; double stepHz = 0.0; std::string reason; };
    StepDecision requestStep(double currentStepHz, double requestedStepHz) const {
        StepDecision d;
        d.stepHz = currentStepHz;
        if (!std::isfinite(requestedStepHz) || requestedStepHz < 1.0 || requestedStepHz > 1e7) { d.allowed = false; d.reason = "not a valid step"; return d; }
        if (stepLocked_) { d.allowed = false; d.reason = "tuning step locked"; return d; }
        d.stepHz = requestedStepHz;
        return d;
    }

    // One step up (+1) or down (-1) through a list of steps (sorted ascending, finite, in range); the lock keeps the current one. Returns the new step.
    StepDecision stepThrough(const std::vector<double>& steps, double currentStepHz, int direction) const {
        StepDecision d;
        d.stepHz = currentStepHz;
        if (steps.empty() || (direction != 1 && direction != -1)) { d.allowed = false; d.reason = "no step list"; return d; }
        if (stepLocked_) { d.allowed = false; d.reason = "tuning step locked"; return d; }
        // the nearest step in the list to the current one, then one further
        std::size_t best = 0;
        double bestDist = std::fabs(steps[0] - currentStepHz);
        for (std::size_t i = 1; i < steps.size(); ++i) { const double dist = std::fabs(steps[i] - currentStepHz); if (dist < bestDist) { bestDist = dist; best = i; } }
        const long next = static_cast<long>(best) + direction;
        const std::size_t idx = static_cast<std::size_t>(std::min<long>(static_cast<long>(steps.size()) - 1, std::max<long>(0, next)));
        if (!std::isfinite(steps[idx]) || steps[idx] < 1.0) { d.allowed = false; d.reason = "not a valid step"; return d; }
        d.stepHz = steps[idx];
        return d;
    }

    nlohmann::json toJson() const {
        nlohmann::json j;
        j["frequencyLocked"] = freqLocked_; j["stepLocked"] = stepLocked_;
        nlohmann::json b = nlohmann::json::array();
        for (bool v : policy_.blocked) { b.push_back(v); }
        j["blocked"] = b;
        return j;
    }
    // Strict: wrong types or a wrong-length list leave the object unchanged and return false.
    bool fromJson(const nlohmann::json& j) {
        if (!j.is_object()) { return false; }
        bool f = freqLocked_, s = stepLocked_;
        LockPolicy pol = policy_;
        if (j.contains("frequencyLocked")) { if (!j["frequencyLocked"].is_boolean()) { return false; } f = j["frequencyLocked"].get<bool>(); }
        if (j.contains("stepLocked")) { if (!j["stepLocked"].is_boolean()) { return false; } s = j["stepLocked"].get<bool>(); }
        if (j.contains("blocked")) {
            const auto& b = j["blocked"];
            if (!b.is_array() || b.size() != static_cast<std::size_t>(kTuneSourceCount)) { return false; }
            for (std::size_t i = 0; i < b.size(); ++i) { if (!b[i].is_boolean()) { return false; } pol.blocked[i] = b[i].get<bool>(); }
        }
        freqLocked_ = f; stepLocked_ = s; policy_ = pol;
        return true;
    }

private:
    bool freqLocked_ = false, stepLocked_ = false;
    LockPolicy policy_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 2. Band-stacking registers.
// ---------------------------------------------------------------------------------------------------------------------------------------------
// What a register remembers: the frequency, the mode and the filter.
struct TuneState {
    double hz = 0.0;
    std::string mode;                     // a mode name such as "USB"
    double bandwidthHz = 0.0;             // the channel bandwidth; 0 = the mode's default
    bool hasEdges = false;
    double lowCutHz = 0.0, highCutHz = 0.0;
    bool operator==(const TuneState& o) const { return hz == o.hz && mode == o.mode && bandwidthHz == o.bandwidthHz && hasEdges == o.hasEdges && lowCutHz == o.lowCutHz && highCutHz == o.highCutHz; }
};

inline bool validTuneState(const TuneState& s) {
    return validFrequency(s.hz) && s.mode.size() <= 16 && std::isfinite(s.bandwidthHz) && s.bandwidthHz >= 0.0 && s.bandwidthHz <= 1e9 &&
           (!s.hasEdges || (std::isfinite(s.lowCutHz) && std::isfinite(s.highCutHz) && std::fabs(s.lowCutHz) <= 1e9 && std::fabs(s.highCutHz) <= 1e9));
}

struct BandDef {
    std::string name;                     // "40 m"
    double lowHz = 0.0, highHz = 0.0;     // inclusive
    std::array<TuneState, 3> defaults{};  // what the three registers hold before the operator has been there
};

inline constexpr int kRegistersPerBand = 3;

class BandStacking {
public:
    // The band plan (not sorted: the first band that contains the frequency wins, so overlapping bands are the plan author's choice).
    bool setBands(const std::vector<BandDef>& bands, std::string* err = nullptr) {
        for (const BandDef& b : bands) {
            if (b.name.empty() || b.name.size() > 32 || !validFrequency(b.lowHz) || !validFrequency(b.highHz) || b.highHz < b.lowHz) { if (err != nullptr) { *err = "bad band '" + b.name + "'"; } return false; }
            for (const TuneState& d : b.defaults) { if (!validTuneState(d)) { if (err != nullptr) { *err = "bad default in band '" + b.name + "'"; } return false; } }
        }
        bands_ = bands;
        regs_.assign(bands_.size(), BandRegs{});
        for (std::size_t i = 0; i < bands_.size(); ++i) { reset(static_cast<int>(i)); }
        return true;
    }

    int bandCount() const { return static_cast<int>(bands_.size()); }
    const BandDef* band(int i) const { return i >= 0 && i < bandCount() ? &bands_[static_cast<std::size_t>(i)] : nullptr; }

    // The index of the band containing `hz`, or -1.
    int bandOf(double hz) const {
        if (!std::isfinite(hz)) { return -1; }
        for (std::size_t i = 0; i < bands_.size(); ++i) { if (hz >= bands_[i].lowHz && hz <= bands_[i].highHz) { return static_cast<int>(i); } }
        return -1;
    }

    // The operator changed frequency, mode or filter: the register currently active in that state's band tracks it. A state outside every band is
    // not remembered (returns false).
    bool track(const TuneState& s) {
        if (!validTuneState(s)) { return false; }
        const int b = bandOf(s.hz);
        if (b < 0) { return false; }
        BandRegs& r = regs_[static_cast<std::size_t>(b)];
        r.reg[static_cast<std::size_t>(r.active)] = s;
        r.valid[static_cast<std::size_t>(r.active)] = true;
        currentBand_ = b;
        return true;
    }

    // The band button was pressed. If the operator is NOT in that band, the band's active register is recalled (where they last were). If they are
    // already in it, the next valid register is recalled (the cycle: 0 -> 1 -> 2 -> 0). Returns the state to tune to, or nullopt for an unknown band.
    std::optional<TuneState> pressBand(int bandIndex) {
        if (bandIndex < 0 || bandIndex >= bandCount()) { return std::nullopt; }
        BandRegs& r = regs_[static_cast<std::size_t>(bandIndex)];
        if (currentBand_ == bandIndex) {
            for (int k = 1; k <= kRegistersPerBand; ++k) {
                const int cand = (r.active + k) % kRegistersPerBand;
                if (r.valid[static_cast<std::size_t>(cand)]) { r.active = cand; break; }
            }
        }
        currentBand_ = bandIndex;
        return r.reg[static_cast<std::size_t>(r.active)];
    }

    // Explicit store / read of one register.
    bool store(int bandIndex, int reg, const TuneState& s) {
        if (bandIndex < 0 || bandIndex >= bandCount() || reg < 0 || reg >= kRegistersPerBand || !validTuneState(s)) { return false; }
        BandRegs& r = regs_[static_cast<std::size_t>(bandIndex)];
        r.reg[static_cast<std::size_t>(reg)] = s;
        r.valid[static_cast<std::size_t>(reg)] = true;
        return true;
    }
    std::optional<TuneState> read(int bandIndex, int reg) const {
        if (bandIndex < 0 || bandIndex >= bandCount() || reg < 0 || reg >= kRegistersPerBand) { return std::nullopt; }
        return regs_[static_cast<std::size_t>(bandIndex)].reg[static_cast<std::size_t>(reg)];
    }
    int activeRegister(int bandIndex) const { return bandIndex >= 0 && bandIndex < bandCount() ? regs_[static_cast<std::size_t>(bandIndex)].active : -1; }
    int currentBand() const { return currentBand_; }

    // Back to the plan's defaults for one band (the active register becomes 0).
    void reset(int bandIndex) {
        if (bandIndex < 0 || bandIndex >= bandCount()) { return; }
        BandRegs& r = regs_[static_cast<std::size_t>(bandIndex)];
        const BandDef& d = bands_[static_cast<std::size_t>(bandIndex)];
        for (std::size_t k = 0; k < static_cast<std::size_t>(kRegistersPerBand); ++k) {
            r.reg[k] = d.defaults[k];
            if (r.reg[k].hz == 0.0) { r.reg[k].hz = (d.lowHz + d.highHz) / 2.0; }          // a default left empty: the middle of the band
            r.valid[k] = true;
        }
        r.active = 0;
    }

    // Persistence. toJson writes names and registers; fromJson matches bands BY NAME (a changed plan does not shift registers onto the wrong band), refuses
    // wrong types, out-of-band frequencies and bad lengths, and changes nothing on failure.
    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::array();
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            nlohmann::json b;
            b["name"] = bands_[i].name; b["active"] = regs_[i].active;
            nlohmann::json rs = nlohmann::json::array();
            for (std::size_t k = 0; k < static_cast<std::size_t>(kRegistersPerBand); ++k) {
                nlohmann::json o;
                o["hz"] = regs_[i].reg[k].hz; o["mode"] = regs_[i].reg[k].mode; o["bw"] = regs_[i].reg[k].bandwidthHz; o["valid"] = regs_[i].valid[k];
                if (regs_[i].reg[k].hasEdges) { o["lo"] = regs_[i].reg[k].lowCutHz; o["hi"] = regs_[i].reg[k].highCutHz; }
                rs.push_back(o);
            }
            b["registers"] = rs;
            j.push_back(b);
        }
        return j;
    }
    bool fromJson(const nlohmann::json& j) {
        if (!j.is_array() || j.size() > 1024) { return false; }
        std::vector<BandRegs> next = regs_;
        for (const auto& b : j) {
            if (!b.is_object() || !b.contains("name") || !b["name"].is_string() || !b.contains("registers") || !b["registers"].is_array() || b["registers"].size() != static_cast<std::size_t>(kRegistersPerBand)) { return false; }
            int idx = -1;
            for (std::size_t i = 0; i < bands_.size(); ++i) { if (bands_[i].name == b["name"].get<std::string>()) { idx = static_cast<int>(i); break; } }
            if (idx < 0) { continue; }   // a band that is no longer in the plan: ignored
            BandRegs r = next[static_cast<std::size_t>(idx)];
            if (b.contains("active")) { if (!b["active"].is_number_integer()) { return false; } const long a = b["active"].get<long>(); if (a < 0 || a >= kRegistersPerBand) { return false; } r.active = static_cast<int>(a); }
            for (std::size_t k = 0; k < static_cast<std::size_t>(kRegistersPerBand); ++k) {
                const auto& o = b["registers"][k];
                if (!o.is_object() || !o.contains("hz") || !o["hz"].is_number() || !o.contains("mode") || !o["mode"].is_string()) { return false; }
                TuneState s;
                s.hz = o["hz"].get<double>(); s.mode = o["mode"].get<std::string>();
                if (o.contains("bw")) { if (!o["bw"].is_number()) { return false; } s.bandwidthHz = o["bw"].get<double>(); }
                if (o.contains("lo") && o.contains("hi")) { if (!o["lo"].is_number() || !o["hi"].is_number()) { return false; } s.hasEdges = true; s.lowCutHz = o["lo"].get<double>(); s.highCutHz = o["hi"].get<double>(); }
                bool valid = true;
                if (o.contains("valid")) { if (!o["valid"].is_boolean()) { return false; } valid = o["valid"].get<bool>(); }
                if (!validTuneState(s)) { return false; }
                const BandDef& d = bands_[static_cast<std::size_t>(idx)];
                if (s.hz < d.lowHz || s.hz > d.highHz) { return false; }          // a register outside its own band is corrupt
                r.reg[k] = s; r.valid[k] = valid;
            }
            if (!r.valid[static_cast<std::size_t>(r.active)]) { return false; }
            next[static_cast<std::size_t>(idx)] = r;
        }
        regs_ = std::move(next);
        return true;
    }

private:
    struct BandRegs {
        std::array<TuneState, 3> reg{};
        std::array<bool, 3> valid{{false, false, false}};
        int active = 0;
    };
    std::vector<BandDef> bands_;
    std::vector<BandRegs> regs_;
    int currentBand_ = -1;
};

// A starting band plan for the owner to adjust (DEFAULT, UNVERIFIED against any band plan document: the edges are the usual amateur and broadcast
// allocations from memory and differ by region, so the wiring lane should take them from the existing World plan data instead).
inline std::vector<BandDef> exampleBandPlan() {
    auto mk = [](const char* n, double lo, double hi, double a, const char* ma, double b, const char* mb, double c, const char* mc) {
        BandDef d; d.name = n; d.lowHz = lo; d.highHz = hi;
        d.defaults[0].hz = a; d.defaults[0].mode = ma; d.defaults[1].hz = b; d.defaults[1].mode = mb; d.defaults[2].hz = c; d.defaults[2].mode = mc;
        return d;
    };
    return {mk("160 m", 1.8e6, 2.0e6, 1.84e6, "LSB", 1.9e6, "LSB", 1.81e6, "CW"), mk("80 m", 3.5e6, 4.0e6, 3.573e6, "USB", 3.7e6, "LSB", 3.55e6, "CW"),
            mk("40 m", 7.0e6, 7.3e6, 7.074e6, "USB", 7.15e6, "LSB", 7.03e6, "CW"), mk("20 m", 14.0e6, 14.35e6, 14.074e6, "USB", 14.25e6, "USB", 14.03e6, "CW"),
            mk("10 m", 28.0e6, 29.7e6, 28.074e6, "USB", 28.4e6, "USB", 28.03e6, "CW"), mk("FM broadcast", 87.5e6, 108.0e6, 94.0e6, "WFM", 98.0e6, "WFM", 104.0e6, "WFM"),
            mk("Airband", 118.0e6, 137.0e6, 121.5e6, "AM", 123.45e6, "AM", 127.0e6, "AM")};
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 3. VFO A/B with swap, copy and split.
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum class VfoId : int { A = 0, B = 1 };

class VfoPair {
public:
    VfoPair() {}

    // `active` is the VFO the knob and the frequency display edit. With split OFF, receive and transmit both use it; with split ON, receive uses A
    // and transmit uses B (the operator listens on one frequency and transmits on another).
    VfoId active() const { return active_; }
    void select(VfoId v) { active_ = v; }
    bool split() const { return split_; }
    void setSplit(bool on) { split_ = on; }

    const TuneState& vfo(VfoId v) const { return v == VfoId::A ? a_ : b_; }
    const TuneState& activeState() const { return vfo(active_); }
    const TuneState& rx() const { return split_ ? a_ : vfo(active_); }
    const TuneState& tx() const { return split_ ? b_ : vfo(active_); }

    // Replaces a VFO's state (validated; false and unchanged when invalid).
    bool set(VfoId v, const TuneState& s) {
        if (!validTuneState(s)) { return false; }
        (v == VfoId::A ? a_ : b_) = s;
        return true;
    }
    bool setActiveHz(double hz) {
        if (!validFrequency(hz)) { return false; }
        (active_ == VfoId::A ? a_ : b_).hz = hz;
        return true;
    }
    void swap() { std::swap(a_, b_); }                   // A <-> B; the selected letter stays selected
    void copyAtoB() { b_ = a_; }
    void copyBtoA() { a_ = b_; }
    void copyActiveToOther() { if (active_ == VfoId::A) { b_ = a_; } else { a_ = b_; } }
    // Split offset in Hz: transmit minus receive when split is on, else 0.
    double splitOffsetHz() const { return split_ ? b_.hz - a_.hz : 0.0; }
    // Puts B at A's frequency plus `offsetHz` (a typical "split up 5 kHz" key) and turns split on. False when the result is not a valid frequency.
    bool splitUp(double offsetHz) {
        if (!std::isfinite(offsetHz) || !validFrequency(a_.hz + offsetHz)) { return false; }
        b_ = a_; b_.hz = a_.hz + offsetHz; split_ = true;
        return true;
    }

    nlohmann::json toJson() const {
        auto one = [](const TuneState& s) { nlohmann::json o; o["hz"] = s.hz; o["mode"] = s.mode; o["bw"] = s.bandwidthHz; if (s.hasEdges) { o["lo"] = s.lowCutHz; o["hi"] = s.highCutHz; } return o; };
        nlohmann::json j; j["a"] = one(a_); j["b"] = one(b_); j["active"] = active_ == VfoId::A ? "A" : "B"; j["split"] = split_;
        return j;
    }
    bool fromJson(const nlohmann::json& j) {
        if (!j.is_object() || !j.contains("a") || !j.contains("b")) { return false; }
        auto rd = [](const nlohmann::json& o, TuneState* s) {
            if (!o.is_object() || !o.contains("hz") || !o["hz"].is_number() || !o.contains("mode") || !o["mode"].is_string()) { return false; }
            s->hz = o["hz"].get<double>(); s->mode = o["mode"].get<std::string>();
            if (o.contains("bw")) { if (!o["bw"].is_number()) { return false; } s->bandwidthHz = o["bw"].get<double>(); }
            if (o.contains("lo") && o.contains("hi")) { if (!o["lo"].is_number() || !o["hi"].is_number()) { return false; } s->hasEdges = true; s->lowCutHz = o["lo"].get<double>(); s->highCutHz = o["hi"].get<double>(); }
            return validTuneState(*s);
        };
        TuneState a, b;
        if (!rd(j["a"], &a) || !rd(j["b"], &b)) { return false; }
        VfoId act = active_; bool sp = split_;
        if (j.contains("active")) { if (!j["active"].is_string()) { return false; } const std::string s = j["active"].get<std::string>(); if (s == "A") { act = VfoId::A; } else if (s == "B") { act = VfoId::B; } else { return false; } }
        if (j.contains("split")) { if (!j["split"].is_boolean()) { return false; } sp = j["split"].get<bool>(); }
        a_ = a; b_ = b; active_ = act; split_ = sp;
        return true;
    }

private:
    TuneState a_, b_;
    VfoId active_ = VfoId::A;
    bool split_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 4. Per-mode memory of bandwidth, step and AGC.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct ModeSettings {
    double bandwidthHz = 0.0;
    double stepHz = 0.0;
    int agcMode = 0;                      // FOXAPI_AGC_*: 0 Auto, 1 Fast, 2 Medium, 3 Slow, 4 Off (engine's numbering, docs/multi-radio-records/agc-nb-wiring.md)
    bool operator==(const ModeSettings& o) const { return bandwidthHz == o.bandwidthHz && stepHz == o.stepHz && agcMode == o.agcMode; }
};

inline bool validModeSettings(const ModeSettings& s) {
    return std::isfinite(s.bandwidthHz) && s.bandwidthHz >= 0.0 && s.bandwidthHz <= 1e9 && std::isfinite(s.stepHz) && s.stepHz >= 0.0 && s.stepHz <= 1e7 && s.agcMode >= 0 && s.agcMode <= 4;
}

class ModeMemory {
public:
    // DEFAULTS (owner can change; UNVERIFIED as typical values): what a mode starts with before the operator has set it. Mode names are the engine's
    // canonical spellings (dsp/demod.hpp modeName): NFM WFM AM DSB USB LSB CW RAW.
    static ModeSettings builtinDefault(const std::string& mode) {
        if (mode == "NFM") { return {12500.0, 12500.0, 1}; }
        if (mode == "WFM") { return {180000.0, 100000.0, 4}; }
        if (mode == "AM") { return {9000.0, 5000.0, 0}; }
        if (mode == "DSB") { return {6000.0, 1000.0, 0}; }
        if (mode == "USB" || mode == "LSB") { return {2700.0, 100.0, 0}; }
        if (mode == "CW") { return {500.0, 10.0, 1}; }
        if (mode == "RAW") { return {48000.0, 1000.0, 4}; }
        return {3000.0, 1000.0, 0};
    }

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // The operator changed the mode from `from` to `to` while the radio had `current`: remember `current` under `from` and return what to apply for `to`
    // (what was remembered, else the built-in default). With the memory disabled nothing is remembered and `current` is returned unchanged.
    ModeSettings changeMode(const std::string& from, const std::string& to, const ModeSettings& current) {
        if (!enabled_ || from == to || !validModeSettings(current)) { return current; }
        if (!from.empty() && from.size() <= 16) { remember(from, current); }
        const auto it = mem_.find(to);
        return it != mem_.end() ? it->second : builtinDefault(to);
    }
    // The operator changed one of the three while in `mode`: keep it current in the memory (so a restart restores it).
    bool remember(const std::string& mode, const ModeSettings& s) {
        if (mode.empty() || mode.size() > 16 || !validModeSettings(s)) { return false; }
        if (mem_.size() >= 64 && mem_.find(mode) == mem_.end()) { return false; }
        mem_[mode] = s;
        return true;
    }
    std::optional<ModeSettings> recall(const std::string& mode) const { const auto it = mem_.find(mode); if (it == mem_.end()) { return std::nullopt; } return it->second; }
    void forget(const std::string& mode) { mem_.erase(mode); }
    void clear() { mem_.clear(); }
    std::size_t size() const { return mem_.size(); }

    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& kv : mem_) { j[kv.first] = {{"bw", kv.second.bandwidthHz}, {"step", kv.second.stepHz}, {"agc", kv.second.agcMode}}; }
        return j;
    }
    bool fromJson(const nlohmann::json& j) {
        if (!j.is_object() || j.size() > 64) { return false; }
        std::map<std::string, ModeSettings> next;
        for (auto it = j.begin(); it != j.end(); ++it) {
            const auto& o = it.value();
            if (it.key().empty() || it.key().size() > 16 || !o.is_object() || !o.contains("bw") || !o["bw"].is_number() || !o.contains("step") || !o["step"].is_number() || !o.contains("agc") || !o["agc"].is_number_integer()) { return false; }
            ModeSettings s{o["bw"].get<double>(), o["step"].get<double>(), static_cast<int>(std::max<long>(-1, std::min<long>(99, o["agc"].get<long>())))};
            if (!validModeSettings(s)) { return false; }
            next[it.key()] = s;
        }
        mem_ = std::move(next);
        return true;
    }

private:
    bool enabled_ = true;
    std::map<std::string, ModeSettings> mem_;
};

}  // namespace cascade::core::aids
