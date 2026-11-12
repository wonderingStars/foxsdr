// FAVOURITES THAT SAVE THE WHOLE STATE: a named snapshot of what the operator sees and hears, recalled in one action.
//
// A bookmark remembers a frequency, a mode and a bandwidth. A favourite also remembers how the screen and the DSP were set for that signal: the display
// range (centre and span of the picture), the spectrum range (top level and span in dB), the waterfall contrast (floor and ceiling in dB), and the DSP
// (AGC mode, noise reduction, noise blanker, notch, squelch, de-emphasis). The recall is the caller's: this header holds the data, the validation and
// the saved form; it applies nothing to a radio.
//
// FavouriteState is plain data with a validity rule (every number finite and in range, every name a user-text of bounded size); FavouriteStore is a
// named list (at most 200, names unique, case-sensitive) with versioned JSON, {"version":1,"favourites":[{...}]}. Another version, a damaged or
// invalid entry, or a repeated name refuses the WHOLE file and changes nothing. A store holds nothing but what the user set.
//
// THREADS: one thread. No file access here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

struct DspState {
    std::string agc = "medium";       // a mode name: off, slow, medium, fast (any name of up to 8 letters is accepted: the engine owns the list)
    bool noiseReduction = false;
    double noiseReductionLevel = 0.5; // 0..1
    bool noiseBlanker = false;
    bool notch = false;
    double squelchDb = -120.0;        // the squelch slider's span, -120..0
    std::string deemphasis = "off";   // off, 50us, 75us (any short alphanumeric name)
};

struct FavouriteState {
    std::string name;
    double freqHz = 100e6;
    std::string mode = "WFM";
    double bandwidthHz = 150000.0;
    double displayCentreHz = 100e6;   // the picture's centre
    double displaySpanHz = 2.4e6;     // the picture's span
    double spectrumTopDb = -20.0;
    double spectrumRangeDb = 100.0;   // the span of the vertical axis
    double waterfallFloorDb = -110.0;
    double waterfallCeilingDb = -30.0;
    DspState dsp;
};

inline bool validDspState(const DspState& d) {
    return validModeName(d.agc) && validModeName(d.deemphasis) && std::isfinite(d.noiseReductionLevel) && d.noiseReductionLevel >= 0.0 && d.noiseReductionLevel <= 1.0 &&
           std::isfinite(d.squelchDb) && d.squelchDb >= -120.0 && d.squelchDb <= 0.0;
}

inline bool validFavourite(const FavouriteState& f) {
    const auto hz = [](double v) { return std::isfinite(v) && v > 0.0 && v <= 1e13; };
    const auto db = [](double v) { return std::isfinite(v) && v >= -200.0 && v <= 100.0; };
    return !f.name.empty() && validUserText(f.name, 64) && hz(f.freqHz) && validModeName(f.mode) && std::isfinite(f.bandwidthHz) && f.bandwidthHz >= 50.0 &&
           f.bandwidthHz <= 1e7 && hz(f.displayCentreHz) && std::isfinite(f.displaySpanHz) && f.displaySpanHz >= 100.0 && f.displaySpanHz <= 1e10 && db(f.spectrumTopDb) &&
           std::isfinite(f.spectrumRangeDb) && f.spectrumRangeDb >= 10.0 && f.spectrumRangeDb <= 200.0 && db(f.waterfallFloorDb) && db(f.waterfallCeilingDb) &&
           f.waterfallCeilingDb > f.waterfallFloorDb && validDspState(f.dsp);
}

inline nlohmann::json favouriteToJson(const FavouriteState& f) {
    return {{"name", f.name}, {"freqHz", f.freqHz}, {"mode", f.mode}, {"bandwidthHz", f.bandwidthHz}, {"displayCentreHz", f.displayCentreHz},
            {"displaySpanHz", f.displaySpanHz}, {"spectrumTopDb", f.spectrumTopDb}, {"spectrumRangeDb", f.spectrumRangeDb},
            {"waterfallFloorDb", f.waterfallFloorDb}, {"waterfallCeilingDb", f.waterfallCeilingDb},
            {"dsp", {{"agc", f.dsp.agc}, {"noiseReduction", f.dsp.noiseReduction}, {"noiseReductionLevel", f.dsp.noiseReductionLevel},
                     {"noiseBlanker", f.dsp.noiseBlanker}, {"notch", f.dsp.notch}, {"squelchDb", f.dsp.squelchDb}, {"deemphasis", f.dsp.deemphasis}}}};
}

// Strict: every field present with the right type and a valid value; false (out untouched) otherwise.
inline bool favouriteFromJson(const nlohmann::json& j, FavouriteState& out, std::string* err = nullptr) {
    auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
    if (!j.is_object()) { return fail("a favourite is not an object"); }
    FavouriteState f;
    const auto text = [&](const char* k, std::string& dst) { return j.contains(k) && j[k].is_string() && ((dst = j[k].get<std::string>()), true); };
    const auto num = [&](const char* k, double& dst) { return j.contains(k) && j[k].is_number() && ((dst = j[k].get<double>()), true); };
    if (!text("name", f.name) || !text("mode", f.mode) || !num("freqHz", f.freqHz) || !num("bandwidthHz", f.bandwidthHz) || !num("displayCentreHz", f.displayCentreHz) ||
        !num("displaySpanHz", f.displaySpanHz) || !num("spectrumTopDb", f.spectrumTopDb) || !num("spectrumRangeDb", f.spectrumRangeDb) ||
        !num("waterfallFloorDb", f.waterfallFloorDb) || !num("waterfallCeilingDb", f.waterfallCeilingDb)) {
        return fail("a favourite field is missing or of the wrong type");
    }
    if (!j.contains("dsp") || !j["dsp"].is_object()) { return fail("dsp is missing"); }
    const nlohmann::json& d = j["dsp"];
    if (!d.contains("agc") || !d["agc"].is_string() || !d.contains("deemphasis") || !d["deemphasis"].is_string() || !d.contains("noiseReduction") ||
        !d["noiseReduction"].is_boolean() || !d.contains("noiseBlanker") || !d["noiseBlanker"].is_boolean() || !d.contains("notch") || !d["notch"].is_boolean() ||
        !d.contains("noiseReductionLevel") || !d["noiseReductionLevel"].is_number() || !d.contains("squelchDb") || !d["squelchDb"].is_number()) {
        return fail("a dsp field is missing or of the wrong type");
    }
    f.dsp.agc = d["agc"].get<std::string>();
    f.dsp.deemphasis = d["deemphasis"].get<std::string>();
    f.dsp.noiseReduction = d["noiseReduction"].get<bool>();
    f.dsp.noiseBlanker = d["noiseBlanker"].get<bool>();
    f.dsp.notch = d["notch"].get<bool>();
    f.dsp.noiseReductionLevel = d["noiseReductionLevel"].get<double>();
    f.dsp.squelchDb = d["squelchDb"].get<double>();
    if (!validFavourite(f)) { return fail("a favourite value is out of range"); }
    out = std::move(f);
    return true;
}

class FavouriteStore {
public:
    static constexpr std::size_t kMaxEntries = 200;

    // Adds or replaces by name. false (changing nothing) for an invalid favourite or a 201st name.
    bool set(const FavouriteState& f, std::string* err = nullptr) {
        if (!validFavourite(f)) { return fail(err, "the favourite is not valid"); }
        if (m_.find(f.name) == m_.end() && m_.size() >= kMaxEntries) { return fail(err, "more than 200 favourites"); }
        m_[f.name] = f;
        return true;
    }
    const FavouriteState* find(const std::string& name) const {
        const auto it = m_.find(name);
        return it == m_.end() ? nullptr : &it->second;
    }
    bool remove(const std::string& name) { return m_.erase(name) > 0; }
    bool rename(const std::string& from, const std::string& to) {
        if (to.empty() || !validUserText(to, 64)) { return false; }
        const auto it = m_.find(from);
        if (it == m_.end() || (from != to && m_.count(to) != 0)) { return false; }
        FavouriteState f = it->second;
        f.name = to;
        m_.erase(it);
        m_[to] = std::move(f);
        return true;
    }
    std::vector<std::string> names() const {
        std::vector<std::string> n;
        for (const auto& e : m_) { n.push_back(e.first); }
        return n;
    }
    std::size_t size() const { return m_.size(); }

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : m_) { arr.push_back(favouriteToJson(e.second)); }
        return {{"version", 1}, {"favourites", arr}};
    }
    bool fromJson(const nlohmann::json& j, std::string* err = nullptr) {
        if (!j.is_object()) { return fail(err, "not an object"); }
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail(err, "unknown version"); }
        if (!j.contains("favourites") || !j["favourites"].is_array()) { return fail(err, "favourites missing"); }
        if (j["favourites"].size() > kMaxEntries) { return fail(err, "more than 200 favourites"); }
        std::map<std::string, FavouriteState> next;
        for (const auto& e : j["favourites"]) {
            FavouriteState f;
            if (!favouriteFromJson(e, f, err)) { return false; }
            if (!next.emplace(f.name, f).second) { return fail(err, "a name is repeated"); }
        }
        m_ = std::move(next);
        return true;
    }

private:
    static bool fail(std::string* err, const char* m) { if (err != nullptr) { *err = m; } return false; }
    std::map<std::string, FavouriteState> m_;
};

}  // namespace cascade::core::tuning
