// PER-BAND MEMORY: what the operator last set on a band comes back when the band is entered again - volume, AGC mode, attenuator, spectrum limits, zoom and
// antenna.
//
// engine.cpp (followBandModeOnTune) says it plainly: nothing is stored per band; the mode follows a band switch by a rule and everything else stays where it
// was. This model is the missing store. It is NOT the band-stacking register (frequency, mode and filter, in feat/recording-models' operating_aids.hpp) and
// not the per-mode memory (bandwidth, step, AGC by mode): it keeps the settings that belong to where you are listening, by the band's NAME, a string the
// caller takes from its band plan (BandEntry name) - the model has no band edges of its own, so it follows whichever plan is loaded.
//
// Each of the six settings is optional: only what was set is stored and recalled, and a mask (setEnabledFields) turns whole kinds off, so an operator who wants
// the antenna remembered and not the volume gets exactly that. onBandChange(from, to, current) is the one call the band switch makes: it files `current`
// under the band being left and returns what to apply for the band being entered (nothing when the band has no memory, or the switch stays in one band).
//
// VALIDITY: volume 0..1; agc a mode name (1..8 letters, digits, + -); attenuator 0..120 dB; spectrum top -200..100 dB and range 10..200 dB (both or neither is
// not required); zoom span 100 Hz..10 GHz; antenna 1..32 bytes of user text without a line break; band name 1..48 bytes of user text. An invalid value
// refuses the whole call and changes nothing. At most 200 bands.
//
// SAVED as versioned JSON (version 1); another version, a damaged entry or an invalid value refuses the WHOLE file. What is stored: the band's name and the six
// settings, nothing else (an antenna's name is a port name such as "Antenna A", typed by the user or given by the hardware).
//
// THREADS: one thread (the control thread). No file access here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

struct BandSettings {
    std::optional<double> volume;        // 0..1
    std::optional<std::string> agc;      // a mode name
    std::optional<int> attenuatorDb;     // 0..120
    std::optional<double> spectrumTopDb;
    std::optional<double> spectrumRangeDb;
    std::optional<double> zoomSpanHz;
    std::optional<std::string> antenna;

    bool empty() const { return !volume && !agc && !attenuatorDb && !spectrumTopDb && !spectrumRangeDb && !zoomSpanHz && !antenna; }
    bool operator==(const BandSettings& o) const {
        return volume == o.volume && agc == o.agc && attenuatorDb == o.attenuatorDb && spectrumTopDb == o.spectrumTopDb && spectrumRangeDb == o.spectrumRangeDb &&
               zoomSpanHz == o.zoomSpanHz && antenna == o.antenna;
    }
};

enum BandField : unsigned {
    kBandVolume = 1u, kBandAgc = 2u, kBandAttenuator = 4u, kBandSpectrum = 8u, kBandZoom = 16u, kBandAntenna = 32u, kBandAllFields = 63u
};

inline bool validBandName(const std::string& n) { return !n.empty() && validUserText(n, 48); }

inline bool validBandSettings(const BandSettings& s) {
    if (s.volume && !(std::isfinite(*s.volume) && *s.volume >= 0.0 && *s.volume <= 1.0)) { return false; }
    if (s.agc && !validModeName(*s.agc)) { return false; }
    if (s.attenuatorDb && (*s.attenuatorDb < 0 || *s.attenuatorDb > 120)) { return false; }
    if (s.spectrumTopDb && !(std::isfinite(*s.spectrumTopDb) && *s.spectrumTopDb >= -200.0 && *s.spectrumTopDb <= 100.0)) { return false; }
    if (s.spectrumRangeDb && !(std::isfinite(*s.spectrumRangeDb) && *s.spectrumRangeDb >= 10.0 && *s.spectrumRangeDb <= 200.0)) { return false; }
    if (s.zoomSpanHz && !(std::isfinite(*s.zoomSpanHz) && *s.zoomSpanHz >= 100.0 && *s.zoomSpanHz <= 1e10)) { return false; }
    if (s.antenna && (s.antenna->empty() || !validUserText(*s.antenna, 32))) { return false; }
    return true;
}

struct BandSwitch {
    bool saved = false;                    // the band left now has a record
    std::optional<BandSettings> apply;     // what to apply for the band entered, already filtered by the mask
};

class BandMemory {
public:
    static constexpr std::size_t kMaxBands = 200;

    void setEnabledFields(unsigned mask) { mask_ = mask & kBandAllFields; }
    unsigned enabledFields() const { return mask_; }

    // Stores the set fields of `s` under the band, keeping the band's other fields. false (nothing changed) for an invalid name or value or a 201st band.
    bool remember(const std::string& band, const BandSettings& s, std::string* err = nullptr) {
        if (!validBandName(band)) { return fail(err, "the band name is not valid"); }
        if (!validBandSettings(s)) { return fail(err, "a setting is out of range"); }
        const BandSettings kept = filtered(s);
        if (kept.empty()) { return true; }
        const auto it = m_.find(band);
        if (it == m_.end() && m_.size() >= kMaxBands) { return fail(err, "more than 200 bands"); }
        BandSettings& dst = m_[band];
        if (kept.volume) { dst.volume = kept.volume; }
        if (kept.agc) { dst.agc = kept.agc; }
        if (kept.attenuatorDb) { dst.attenuatorDb = kept.attenuatorDb; }
        if (kept.spectrumTopDb) { dst.spectrumTopDb = kept.spectrumTopDb; }
        if (kept.spectrumRangeDb) { dst.spectrumRangeDb = kept.spectrumRangeDb; }
        if (kept.zoomSpanHz) { dst.zoomSpanHz = kept.zoomSpanHz; }
        if (kept.antenna) { dst.antenna = kept.antenna; }
        return true;
    }
    // What the band remembers, through the mask; nullopt when nothing.
    std::optional<BandSettings> recall(const std::string& band) const {
        const auto it = m_.find(band);
        if (it == m_.end()) { return std::nullopt; }
        const BandSettings f = filtered(it->second);
        if (f.empty()) { return std::nullopt; }
        return f;
    }
    // The band switch: `current` is the live state (all six set, or those the caller can read). Files it under `from` (when non-empty and not equal to `to`)
    // and returns the memory of `to`.
    BandSwitch onBandChange(const std::string& from, const std::string& to, const BandSettings& current) {
        BandSwitch r;
        if (from == to) { return r; }
        if (!from.empty() && validBandName(from)) { r.saved = remember(from, current) && m_.count(from) != 0; }
        if (!to.empty() && validBandName(to)) { r.apply = recall(to); }
        return r;
    }
    bool forget(const std::string& band) { return m_.erase(band) > 0; }
    void clear() { m_.clear(); }
    std::size_t size() const { return m_.size(); }

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : m_) {
            nlohmann::json o = {{"band", e.first}};
            const BandSettings& s = e.second;
            if (s.volume) { o["volume"] = *s.volume; }
            if (s.agc) { o["agc"] = *s.agc; }
            if (s.attenuatorDb) { o["attenuatorDb"] = *s.attenuatorDb; }
            if (s.spectrumTopDb) { o["spectrumTopDb"] = *s.spectrumTopDb; }
            if (s.spectrumRangeDb) { o["spectrumRangeDb"] = *s.spectrumRangeDb; }
            if (s.zoomSpanHz) { o["zoomSpanHz"] = *s.zoomSpanHz; }
            if (s.antenna) { o["antenna"] = *s.antenna; }
            arr.push_back(o);
        }
        return {{"version", 1}, {"bands", arr}};
    }
    bool fromJson(const nlohmann::json& j, std::string* err = nullptr) {
        if (!j.is_object()) { return fail(err, "not an object"); }
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail(err, "unknown version"); }
        if (!j.contains("bands") || !j["bands"].is_array()) { return fail(err, "bands missing"); }
        if (j["bands"].size() > kMaxBands) { return fail(err, "more than 200 bands"); }
        std::map<std::string, BandSettings> next;
        for (const auto& e : j["bands"]) {
            if (!e.is_object() || !e.contains("band") || !e["band"].is_string()) { return fail(err, "an entry is damaged"); }
            const std::string name = e["band"].get<std::string>();
            BandSettings s;
            auto num = [&](const char* k, std::optional<double>& dst) {
                if (!e.contains(k)) { return true; }
                if (!e[k].is_number()) { return false; }
                dst = e[k].get<double>();
                return true;
            };
            if (!num("volume", s.volume) || !num("spectrumTopDb", s.spectrumTopDb) || !num("spectrumRangeDb", s.spectrumRangeDb) || !num("zoomSpanHz", s.zoomSpanHz)) {
                return fail(err, "a number has the wrong type");
            }
            if (e.contains("agc")) { if (!e["agc"].is_string()) { return fail(err, "agc is not text"); } s.agc = e["agc"].get<std::string>(); }
            if (e.contains("antenna")) { if (!e["antenna"].is_string()) { return fail(err, "antenna is not text"); } s.antenna = e["antenna"].get<std::string>(); }
            if (e.contains("attenuatorDb")) {
                if (!e["attenuatorDb"].is_number_integer()) { return fail(err, "attenuator is not a whole number"); }
                const long long a = e["attenuatorDb"].get<long long>();
                if (a < 0 || a > 120) { return fail(err, "attenuator out of range"); }
                s.attenuatorDb = static_cast<int>(a);
            }
            if (!validBandName(name) || !validBandSettings(s) || s.empty()) { return fail(err, "an entry is not valid"); }
            if (!next.emplace(name, s).second) { return fail(err, "a band is repeated"); }
        }
        m_ = std::move(next);
        return true;
    }

private:
    BandSettings filtered(const BandSettings& s) const {
        BandSettings f;
        if (mask_ & kBandVolume) { f.volume = s.volume; }
        if (mask_ & kBandAgc) { f.agc = s.agc; }
        if (mask_ & kBandAttenuator) { f.attenuatorDb = s.attenuatorDb; }
        if (mask_ & kBandSpectrum) { f.spectrumTopDb = s.spectrumTopDb; f.spectrumRangeDb = s.spectrumRangeDb; }
        if (mask_ & kBandZoom) { f.zoomSpanHz = s.zoomSpanHz; }
        if (mask_ & kBandAntenna) { f.antenna = s.antenna; }
        return f;
    }
    static bool fail(std::string* err, const char* m) { if (err != nullptr) { *err = m; } return false; }
    std::map<std::string, BandSettings> m_;
    unsigned mask_ = kBandAllFields;
};

}  // namespace cascade::core::tuning
