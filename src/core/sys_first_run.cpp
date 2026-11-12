// sys_first_run.cpp - see sys_first_run.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_first_run.hpp"

#include <fstream>

#include "source/airspy_protocol.hpp"
#include "source/airspyhf_protocol.hpp"
#include "source/hackrf_protocol.hpp"
#include "source/rx888_protocol.hpp"
#include "source/tuner_msi001.hpp"

namespace fs = std::filesystem;

namespace cascade::core::sys {
namespace {

using cascade::core::Bookmark;

Bookmark bm(const char* name, double hz, const char* mode, double bw, const char* group, bool fav = false) {
    Bookmark b;
    b.name = name;
    b.freqHz = hz;
    b.mode = mode;
    b.bandwidthHz = bw;
    b.group = group;
    b.favourite = fav;
    return b;
}

std::vector<RadioChoiceInfo> makeChoices() {
    constexpr double kConsLo = 88.0e6, kConsHi = 470.0e6;   // the conservative window for families with no range constant in the source
    namespace as = cascade::source::airspy;
    namespace hf = cascade::source::airspyhf;
    namespace hr = cascade::source::hackrf;
    namespace rx = cascade::source::rx888;
    namespace ms = cascade::source::msi001;
    return {
        {RadioFamily::NoneYet, "siggen", "I have no radio yet (try the built-in test signal)", {}, true},
        {RadioFamily::RtlSdr, "rtlsdr", "RTL-SDR dongle", {{kConsLo, kConsHi}}, false},
        {RadioFamily::Airspy, "airspy", "Airspy R2 / Mini", {{as::kMinFrequencyHz, as::kMaxFrequencyHz}}, true},
        {RadioFamily::AirspyHf, "airspyhf", "Airspy HF+", {{hf::kHfLowHz, hf::kHfHighHz}, {hf::kVhfLowHz, hf::kVhfHighHz}}, true},
        {RadioFamily::Sdrplay, "sdrplay", "SDRplay RSP", {{kConsLo, kConsHi}}, false},
        {RadioFamily::HackRf, "hackrf", "HackRF", {{hr::kMinFrequencyHz, hr::kMaxFrequencyHz}}, true},
        {RadioFamily::Rx888, "rx888", "RX888 mk2", {{rx::kMinFrequencyHz, rx::kMaxFrequencyHz}}, true},
        {RadioFamily::MiriSdr, "mirisdr", "Mirics MSi2500", {{ms::kMinFrequencyHz, ms::kMaxFrequencyHz}}, true},
        {RadioFamily::Pluto, "pluto", "ADALM-Pluto", {{kConsLo, kConsHi}}, false},
        {RadioFamily::IqFile, "file", "A recorded I/Q file", {}, true},
        {RadioFamily::SoundCard, "soundcard", "A sound card input", {}, true},
    };
}

std::vector<StarterPreset> makePresets() {
    std::vector<StarterPreset> p;
    p.push_back({"fm-broadcast", "FM broadcast band", {bm("FM broadcast (band centre)", 98.0e6, "WFM", 150000, "Starter: FM", true)}});
    p.push_back({"airband", "Airband emergency channel", {bm("Airband emergency 121.500", 121.5e6, "AM", 10000, "Starter: Air")}});
    p.push_back({"marine", "Marine VHF channel 16", {bm("Marine VHF 16 (156.800)", 156.8e6, "NFM", 12500, "Starter: Marine")}});
    p.push_back({"pmr446", "PMR446 channel 1", {bm("PMR446 channel 1", 446.00625e6, "NFM", 12500, "Starter: PMR")}});
    p.push_back({"ham-2m", "2 m amateur FM", {bm("2 m FM calling 145.500", 145.5e6, "NFM", 12500, "Starter: Amateur"), bm("APRS Europe 144.800", 144.8e6, "NFM", 12500, "Starter: Amateur")}});
    p.push_back({"time-signals", "Time signals (WWV)", {bm("WWV 5 MHz", 5.0e6, "AM", 6000, "Starter: Time"), bm("WWV 10 MHz", 10.0e6, "AM", 6000, "Starter: Time")}});
    p.push_back({"shortwave-49m", "Shortwave broadcast, 49 m", {bm("49 m broadcast band (6 MHz)", 6.0e6, "AM", 9000, "Starter: Shortwave")}});
    p.push_back({"ham-hf", "Amateur HF", {bm("40 m phone 7.100 LSB", 7.1e6, "LSB", 2700, "Starter: HF"), bm("20 m FT8 14.074 USB", 14.074e6, "USB", 3000, "Starter: HF")}});
    return p;
}

bool fitsWindows(const StarterPreset& p, const std::vector<std::pair<double, double>>& windows) {
    if (windows.empty()) { return false; }
    for (const auto& b : p.bookmarks) {
        bool inside = false;
        for (const auto& w : windows) { if (b.freqHz >= w.first && b.freqHz <= w.second) { inside = true; } }
        if (!inside) { return false; }
    }
    return true;
}

}  // namespace

const std::vector<RadioChoiceInfo>& radioChoices() {
    static const std::vector<RadioChoiceInfo> v = makeChoices();
    return v;
}

const RadioChoiceInfo& radioChoice(RadioFamily f) {
    for (const auto& c : radioChoices()) { if (c.family == f) { return c; } }
    return radioChoices().front();
}

const std::vector<StarterPreset>& allStarterPresets() {
    static const std::vector<StarterPreset> v = makePresets();
    return v;
}

std::vector<StarterPreset> startersFor(RadioFamily f) {
    std::vector<StarterPreset> out;
    const auto& w = radioChoice(f).windowsHz;
    for (const auto& p : allStarterPresets()) { if (fitsWindows(p, w)) { out.push_back(p); } }
    return out;
}

bool FirstRunWizard::canNext() const {
    switch (step_) {
        case WizardStep::Welcome: return true;
        case WizardStep::Radio: return radioChosen_;
        case WizardStep::Preset: return true;   // "none" is a choice
        case WizardStep::Privacy: return telemetrySeen_;
        case WizardStep::Done: return false;
    }
    return false;
}

bool FirstRunWizard::next() {
    if (!canNext()) { return false; }
    step_ = static_cast<WizardStep>(static_cast<int>(step_) + 1);
    return true;
}

bool FirstRunWizard::back() {
    if (step_ == WizardStep::Welcome || step_ == WizardStep::Done) { return false; }
    step_ = static_cast<WizardStep>(static_cast<int>(step_) - 1);
    return true;
}

bool FirstRunWizard::chooseRadio(RadioFamily f) {
    bool known = false;
    for (const auto& c : radioChoices()) { if (c.family == f) { known = true; } }
    if (!known) { return false; }
    const bool changed = !radioChosen_ || radio_ != f;
    radio_ = f;
    radioChosen_ = true;
    if (changed && !presetId_.empty()) {
        // A preset chosen for another radio is kept only if it still fits this one.
        bool still = false;
        for (const auto& p : startersFor(f)) { if (p.id == presetId_) { still = true; } }
        if (!still) { presetId_.clear(); }
    }
    return true;
}

bool FirstRunWizard::choosePreset(const std::string& id) {
    if (id.empty()) { presetId_.clear(); return true; }
    if (!radioChosen_) { return false; }
    for (const auto& p : startersFor(radio_)) { if (p.id == id) { presetId_ = id; return true; } }
    return false;
}

WizardOutcome FirstRunWizard::outcome() const {
    WizardOutcome o;
    if (skipped_ || step_ != WizardStep::Done) { return o; }
    o.applied = true;
    o.sourceKind = radioChoice(radio_).sourceKind;
    o.telemetryEnabled = telemetry_;
    o.presetId = presetId_;
    if (!presetId_.empty()) {
        for (const auto& p : startersFor(radio_)) { if (p.id == presetId_) { o.bookmarks = p.bookmarks; } }
    }
    return o;
}

bool firstRunNeeded(const fs::path& settingsDir) {
    std::error_code ec;
    if (fs::exists(settingsDir / "config.json", ec)) { return false; }
    if (fs::exists(settingsDir / kFirstRunMarker, ec)) { return false; }
    return true;
}

bool markFirstRunDone(const fs::path& settingsDir) {
    std::error_code ec;
    fs::create_directories(settingsDir, ec);
    if (ec) { return false; }
    std::ofstream f(settingsDir / kFirstRunMarker, std::ios::binary | std::ios::trunc);
    f << "1\n";
    f.flush();
    return static_cast<bool>(f);
}

}  // namespace cascade::core::sys
