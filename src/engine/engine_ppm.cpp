// engine_ppm.cpp - the crystal correction ("PPM frequency correction", 0.99.56): the per-radio memory behind the Source section's switch and
// value, and applying the value to the open radio. Engine members (they were AppWindow's in master: src/gui/app_window_ppm.cpp), kept out of
// engine.cpp because they are one subject; the window's half - the controls - reads them as facts (fact.ppm.*) and sends FOXAPP_OP_SET_PPM.
//
// WHAT THIS FILE DOES NOT DO IS ARITHMETIC. core/ppm_correction.hpp holds the conversion and the rules; the source view
// (source/converter_view.hpp) applies the retune fallback; a radio that corrects its own crystal does it itself
// (IqSource::setFrequencyCorrectionPpm). This file decides which value is in force - the open radio's own, under core::ppmRadioKey - and how.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <cmath>
#include <string>

#include "core/diag_log.hpp"
#include "core/ppm_correction.hpp"
#include "core/utf8_text.hpp"

namespace cascade::engine {

namespace cc = cascade::core;

std::string Engine::ppmRadioKeyNow() const { return cc::ppmRadioKey(sourceKind_, deviceArgs_); }

double Engine::ppmForKey(const std::string& key) const {
    return cc::ppmFor(ppmCorrectionOn_, ppmValues_, key);
}

double Engine::ppmForPatchDevice(const std::string& deviceKey) const {
    namespace pc = cascade::core::patch;
    return ppmForKey(cc::ppmRadioKey(pc::deviceDriver(deviceKey), pc::deviceArgs(deviceKey)));
}

cc::PpmMethod Engine::ppmMethodNow() {
    return cc::ppmMethodFor(sourceKind_, pipeline_.rawSource().hasFrequencyCorrection());
}

void Engine::applyPpmForSource() {
    // A RADIO OPENS WITH NO CORRECTION OF ITS OWN, and setSource has already
    // put the pipeline's view back to none. Nothing below runs - nothing at
    // all is sent to the radio - while the switch is off or this radio has no
    // value: off is exactly the behaviour before this existed.
    ppmRadioApplied_ = 0.0;
    ppmError_.clear();
    pipeline_.setSoftwarePpm(0.0);
    const std::optional<PpmPending> pending = ppmPending_;
    ppmPending_.reset();
    const std::string key = ppmRadioKeyNow();
    const double ppm = ppmForKey(key);
    if (ppm == 0.0) { return; }
    const cc::PpmMethod method = ppmMethodNow();
    if (method == cc::PpmMethod::NotApplicable) { return; }
    if (method == cc::PpmMethod::InRadio) {
        // The driver retunes to its own centre with the new reference
        // (RtlSdrSource, SoapySource), so the frequency it reports holds.
        cascade::source::IqSource& raw = pipeline_.rawSource();
        if (raw.setFrequencyCorrectionPpm(ppm)) {
            ppmRadioApplied_ = ppm;
            if (cc::ppmRadioTakesWholePpm(sourceKind_)) {
                cascade::core::diagLogf(
                    "source: frequency correction %s ppm applied in the radio (%s, whole ppm: %+d)",
                    cc::ppmText(ppm).c_str(), sourceKind_.c_str(), cc::ppmWholeForRadio(ppm));
            } else {
                cascade::core::diagLogf("source: frequency correction %s ppm applied in the radio (%s)",
                                        cc::ppmText(ppm).c_str(), sourceKind_.c_str());
            }
        } else {
            ppmError_ = raw.lastError();
            cascade::core::diagWarnf("source: the %s refused its frequency correction (%s)",
                                     sourceKind_.c_str(), ppmError_.c_str());
        }
        return;
    }
    // WHERE THE RADIO ALREADY IS stays where it is. A radio told the corrected
    // frequency before it was installed (ppmPreTellHz) and still there is
    // handed to the view with the memo of that tune, and nothing is sent. Any
    // other radio sits where an UNcorrected tune put it - its driver's default,
    // or a pre-install tune made without the correction - and is moved now so
    // it really is on the frequency it reports.
    if (pending.has_value() && pending->key == key &&
        pending->memo.sentHz == pipeline_.rawSource().centerFrequencyHz()) {
        pipeline_.setSoftwarePpm(ppm, pending->memo);
    } else {
        const std::optional<double> air = carriedAirCentre();
        pipeline_.setSoftwarePpm(ppm);
        if (air.has_value()) { pipeline_.activeSource().setCenterFrequencyHz(*air); }
    }
    cascade::core::diagLogf(
        "source: frequency correction %s ppm applied by retuning (%s, centre frequency only)",
        cc::ppmText(ppm).c_str(), sourceKind_.c_str());
}

double Engine::ppmPreTellHz(const std::string& kind, const std::string& args, bool radioCorrects,
                            double radioHz) {
    ppmPending_.reset();
    const std::string key = cc::ppmRadioKey(kind, args);
    const double ppm = ppmForKey(key);
    if (ppm == 0.0 || cc::ppmMethodFor(kind, radioCorrects) != cc::PpmMethod::Retune) {
        return radioHz;
    }
    const double told = cc::ppmRequestHz(radioHz, ppm);
    ppmPending_ = PpmPending{key, cc::PpmMemo{true, radioHz, told}};
    return told;
}

void Engine::changePpm(bool on, double ppm) {
    const std::string key = ppmRadioKeyNow();
    const double before = ppmForKey(key);
    ppmCorrectionOn_ = on;
    ppm = cc::sanitisePpm(ppm);
    if (!key.empty()) {
        // 0 is what no entry means, so it is kept as no entry.
        if (ppm == 0.0) {
            ppmValues_.erase(key);
        } else if (ppmValues_.count(key) != 0 || ppmValues_.size() < cc::kMaxPpmRadios) {
            ppmValues_[key] = ppm;
        }
    }
    const double after = ppmForKey(key);
    if (after == before) { return; }
    const cc::PpmMethod method = ppmMethodNow();
    if (method == cc::PpmMethod::NotApplicable) { return; }
    if (method == cc::PpmMethod::InRadio) {
        cascade::source::IqSource& raw = pipeline_.rawSource();
        if (raw.setFrequencyCorrectionPpm(after)) {
            ppmRadioApplied_ = after;
            ppmError_.clear();
            cascade::core::diagLogf("source: frequency correction %s ppm in the radio (%s)",
                                    cc::ppmText(after).c_str(), sourceKind_.c_str());
        } else {
            ppmError_ = raw.lastError();
            cascade::core::diagWarnf("source: the %s refused its frequency correction (%s)",
                                     sourceKind_.c_str(), ppmError_.c_str());
        }
        return;
    }
    // BY RETUNING: THE STATION STAYS, THE RADIO FOLLOWS - the same rule as a
    // converter change. The true centre is read through the OLD correction,
    // then the radio is told what the new one makes of it.
    const std::optional<double> air = carriedAirCentre();
    pipeline_.setSoftwarePpm(after);
    tuneMismatchNote_.clear();
    if (air.has_value()) {
        retuneCoalescer_.clearPending();
        applyRetuneNow(*air);
    }
    cascade::core::diagLogf("source: frequency correction %s ppm by retuning (%s)",
                            cc::ppmText(after).c_str(), sourceKind_.c_str());
}

std::string Engine::ppmCardValueText() {
    if (!ppmCorrectionOn_) { return {}; }
    const cc::PpmMethod method = ppmMethodNow();
    if (method == cc::PpmMethod::NotApplicable) { return {}; }
    double shown = ppmForKey(ppmRadioKeyNow());
    // What the radio really took: a whole-ppm register gets the rounded value.
    if (method == cc::PpmMethod::InRadio && cc::ppmRadioTakesWholePpm(sourceKind_)) {
        shown = static_cast<double>(cc::ppmWholeForRadio(shown));
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", shown);
    return buf;
}

std::string Engine::ppmDiagText() {
    if (!ppmCorrectionOn_) { return "off"; }
    const cc::PpmMethod method = ppmMethodNow();
    if (method == cc::PpmMethod::NotApplicable) { return "not applicable"; }
    const std::string value = cc::ppmText(ppmForKey(ppmRadioKeyNow()));
    if (method == cc::PpmMethod::InRadio) {
        return value + (ppmError_.empty() ? " in the radio" : " in the radio (refused)");
    }
    return value + " by retuning";
}

// THE FACTS THE SOURCE SECTION'S PPM CONTROLS DRAW FROM (API 0.3 section 9, fact.ppm.*), one per line "key value"; built on demand by the
// list block (engine_api_lists.cpp). `method`: "na" (nothing to correct), "radio" (in the radio) or "retune".
Engine::PpmFacts Engine::ppmFacts() {
    PpmFacts f;
    f.on = ppmCorrectionOn_;
    const cc::PpmMethod method = ppmMethodNow();
    f.method = method == cc::PpmMethod::NotApplicable ? "na" : (method == cc::PpmMethod::InRadio ? "radio" : "retune");
    const std::string key = ppmRadioKeyNow();
    f.stored = cc::ppmFor(true, ppmValues_, key);
    f.wholeRadio = method == cc::PpmMethod::InRadio && cc::ppmRadioTakesWholePpm(sourceKind_);
    f.wholeValue = cc::ppmWholeForRadio(f.stored);
    f.error = ppmError_;
    f.cardValue = ppmCardValueText();
    // WHICH RADIO the value is for - the serial is what tells two identical dongles apart. On screen only; never logged.
    f.model = deviceModel_.empty() ? std::string(pipeline_.activeSource().name()) : deviceModel_;
    f.serial = cascade::source::argValue(deviceArgs_, "serial");
    f.key = key;
    return f;
}

}  // namespace cascade::engine
