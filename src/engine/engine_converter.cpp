// engine_converter.cpp - the Engine's machinery from gui/app_window_converter.cpp, moved VERBATIM (engine
// extraction stage 3a, docs/engine-stage3.md): each definition is the
// window's, renamed AppWindow:: -> Engine::, with the few lines that reached
// into the window turned into calls on the host (engine_host.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "core/diag_log.hpp"
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "core/patch_devices.hpp"
#include "core/utf8_text.hpp"
#include "engine/soundcard_panel.hpp"
#include "engine/tune_control.hpp"

#include "engine/audio_open.hpp"
#include "engine/bias_tee.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine/rate_follow_status.hpp"
#include "engine/receiver_tables.hpp"
#include "engine/running_view.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tx_frequency.hpp"

namespace cc = cascade::core;

namespace cascade::engine {

// The helpers moved out of src/gui keep their namespace (cascade::gui), so
// the code that moved with them reads exactly as it did.
using namespace cascade::gui;
using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// Sentences joined the way the language writes them: a space after an ASCII
// full stop, none after a CJK one ("。"), which is not followed by a space.
void appendSentence(std::string& msg, const std::string& next) {
    if (next.empty()) { return; }
    if (!msg.empty() && static_cast<unsigned char>(msg.back()) < 0x80u) { msg += ' '; }
    msg += next;
}

}  // namespace

std::string Engine::resolveConverterKey(const std::string& radioKey) const {
    const auto it = converterKeyAlias_.find(radioKey);
    return it == converterKeyAlias_.end() ? radioKey : it->second;
}

std::string Engine::converterRawKeyNow() const {
    // A SOUND CARD BY ITS CARD: it leaves deviceArgs_ empty, and "soundcard|"
    // would give every card one shared setting (gui::soundCardConverterKey).
    if (sourceKind_ == "soundcard") {
        return cascade::gui::soundCardConverterKey(soundCardLive_.device, soundCardLive_.hostApi);
    }
    return cc::converterRadioKey(sourceKind_, deviceArgs_);
}

std::string Engine::converterRadioKeyNow() const {
    return resolveConverterKey(converterRawKeyNow());
}

cc::ConverterSetting Engine::converterForKey(const std::string& radioKey) const {
    // A patch radio names its card by the full name; the card's converter is
    // kept under its identity (gui::soundCardConverterKey - on Linux without
    // the ALSA card number), so the key is brought to that form first.
    // A patch radio playing an I/Q recording (0.99.40) uses the receiver's
    // converter for I/Q files (core::patch::converterKeyForDevice).
    const std::string key = cascade::gui::soundCardCanonicalKey(
        resolveConverterKey(cascade::core::patch::converterKeyForDevice(radioKey)));
    const cc::ConverterSetting stored = cc::converterFor(converters_, key);
    // What a sound card can have in front of it depends on its format
    // (gui::soundCardConverter): nothing for I/Q, a down-converter for real.
    std::string cardArgs;
    if (cascade::gui::soundCardKeyArgs(key, cardArgs)) {
        const cascade::source::SoundCardFormat f = cascade::gui::soundCardFormatForKey(
            cardArgs, sourceKind_ == "soundcard", soundCardLive_, soundCard_);
        return cascade::gui::soundCardConverter(stored, f).effective;
    }
    return stored;
}

void Engine::noteConverterFallback(const std::string& nativeKey,
                                      const std::string& fallbackKey) {
    converterNotCarried_.erase(fallbackKey);
    if (nativeKey.empty() || nativeKey == fallbackKey) { return; }
    // A Soapy key with a converter of its own keeps it: the user set that one
    // for this very way of reaching the dongle. ONLY AN ACTIVE ONE counts - a
    // record left OFF (changeConverter stores Off too, to remember the LO) is
    // "none", and must not silently stop the radio's own converter applying.
    if (cc::converterActive(cc::converterFor(converters_, fallbackKey))) {
        converterKeyAlias_.erase(fallbackKey);
        return;
    }
    // ONLY THE SAME DONGLE, PROVABLY (gui::fallbackNamesTheSameDongle): with
    // no serial in the Soapy args SoapySDR may have opened a different dongle
    // from the one the native row named. Then nothing is carried, and the
    // Source section says so when the native radio had a converter to carry.
    if (!cascade::gui::fallbackNamesTheSameDongle(nativeKey, fallbackKey)) {
        converterKeyAlias_.erase(fallbackKey);
        if (cc::converterActive(cc::converterFor(converters_, nativeKey))) {
            converterNotCarried_.insert(fallbackKey);
            cascade::core::diagLogf("source: the SoapySDR fallback names no serial the native "
                                    "radio matches; its converter is not carried");
        }
        return;
    }
    // EDITS MADE WHILE THIS ALIAS IS IN FORCE ARE STORED UNDER THE NATIVE KEY
    // ONLY (converterRadioKeyNow resolves through it), never under the Soapy
    // one. Accepted: the dongle is the native radio, the native key is where
    // the next native open looks, and a later fallback aliases again and
    // finds the edit there. The Soapy key simply never gets a record of its
    // own this way.
    converterKeyAlias_[fallbackKey] = nativeKey;
    const cc::ConverterSetting s = converterForKey(fallbackKey);
    // Which way it is set, never a frequency (see changeConverter).
    cascade::core::diagLogf("source: the SoapySDR fallback keeps this radio's converter (%s%s)",
                            cc::converterModeKey(cc::converterActive(s) ? s.mode
                                                                        : cc::ConverterMode::Off),
                            s.inverted && cc::converterActive(s) ? ", inverted" : "");
}

std::optional<double> Engine::carriedAirCentre() {
    // AT THE RADIO, because that is where "no frequency" shows: a device that
    // was never tuned reads 0 Hz. The AIR figure is what is carried, and it
    // may be below 0 Hz (a VLF station with the VFO parked up, through an
    // up-converter) - which is a frequency, not the absence of one.
    if (!(pipeline_.rawSource().centerFrequencyHz() > 0.0)) { return std::nullopt; }
    return pipeline_.activeSource().centerFrequencyHz();
}

void Engine::applyConverterForSource() {
    pipeline_.setConverter(converterForKey(converterRadioKeyNow()));
    converterHeldAir_.reset();   // a station held for the radio just replaced
    // The LO field re-seeds from the radio now installed.
    host_->onConverterChanged(true);
}

double Engine::radioHzForSource(const std::string& kind, const std::string& args,
                                   double airHz) const {
    return cc::radioFromAir(converterForKey(cc::converterRadioKey(kind, args)), airHz);
}

std::string Engine::converterName(const cc::ConverterSetting& s) const {
    const std::string lo = cc::converterHzText(s.loHz);
    std::string out;
    if (s.mode == cc::ConverterMode::Down) {
        cascade::core::formatUtf8(out,
                                  s.inverted ? tr("%s down-converter, spectrum inverted")
                                             : tr("%s down-converter"),
                                  lo.c_str());
    } else {
        cascade::core::formatUtf8(out,
                                  s.inverted ? tr("%s up-converter, spectrum inverted")
                                             : tr("%s up-converter"),
                                  lo.c_str());
    }
    return out;
}

std::string Engine::converterTuneNote(double requestAirHz, bool refused, double answeredAirHz,
                                         bool isPluginPreset) {
    const cc::ConverterSetting conv = pipeline_.converter();
    if (!cc::converterActive(conv)) { return {}; }
    double rLo = 0.0;
    double rHi = 0.0;
    const bool hasRange = device_ != nullptr && device_->frequencyRangeHz(rLo, rHi);
    const std::string name = converterName(conv);
    const std::string air = cc::converterHzText(requestAirHz);
    const double radioHz = cc::radioFromAir(conv, requestAirHz);

    std::string msg;
    if (!cc::airReachable(conv, requestAirHz)) {
        // The radio was never asked: the frequency it would need is 0 Hz or
        // below. Said whether or not the radio publishes a range - this one
        // is the converter's limit, not the radio's.
        cascade::core::formatUtf8(
            msg, tr("%s is out of reach through the %s: the radio would have to tune to 0 Hz or below."),
            air.c_str(), name.c_str());
    } else if (refused) {
        // The same rule as the plain sentence (engine/tune_control.hpp,
        // tuneRefusedMessage): a refusal INSIDE the radio's range is a busy
        // driver, not a fact about the radio, and says nothing.
        if (!hasRange || !(rHi > rLo) || (radioHz >= rLo && radioHz <= rHi)) { return {}; }
        cascade::core::formatUtf8(
            msg, tr("This radio cannot tune to %s through the %s (%s at the radio) - it stayed where it was."),
            air.c_str(), name.c_str(), cc::converterHzText(radioHz).c_str());
    } else {
        if (std::fabs(answeredAirHz - requestAirHz) <= cascade::gui::kTuneMismatchToleranceHz) {
            return {};
        }
        cascade::core::formatUtf8(
            msg, tr("This radio cannot tune to %s through the %s (%s at the radio) - it answered %s."),
            air.c_str(), name.c_str(), cc::converterHzText(radioHz).c_str(),
            cc::converterHzText(answeredAirHz).c_str());
    }
    // WHAT IS REACHABLE, in air terms: the radio's range seen through the
    // converter (or the converter's own limit when the radio publishes none).
    double aLo = 0.0;
    double aHi = 0.0;
    if (cc::airRangeHz(conv, hasRange, rLo, rHi, aLo, aHi)) {
        std::string range;
        if (std::isinf(aHi)) {
            cascade::core::formatUtf8(range, tr("Through the converter this radio reaches %s and up."),
                                      cc::converterHzText(aLo).c_str());
        } else {
            cascade::core::formatUtf8(range, tr("Through the converter this radio reaches %s to %s."),
                                      cc::converterHzText(aLo).c_str(),
                                      cc::converterHzText(aHi).c_str());
        }
        appendSentence(msg, range);
    }
    if (isPluginPreset) {
        appendSentence(msg, tr("This preset needs a receiver that covers that band."));
    }
    return msg;
}

void Engine::changeConverter(const cc::ConverterSetting& s) {
    const std::string key = converterRadioKeyNow();
    std::optional<double> airBefore = carriedAirCentre();
    // THE STATION A RELABEL COULD NOT KEEP (see below), while the radio has
    // not moved since: switching a converter off from 17.2 kHz leaves the
    // dongle on 125.0172 MHz and the counter reading that, and switching it
    // straight back on must find 17.2 kHz again - not keep 125.0172 MHz on
    // the air and send the radio to 250.0172 MHz. A tune moves the radio
    // (and a source install clears this), so any later change starts afresh.
    const double radioNowHz = pipeline_.rawSource().centerFrequencyHz();
    if (converterHeldAir_.has_value() && converterHeldAir_->key == key &&
        converterHeldAir_->radioHz == radioNowHz && airBefore.has_value()) {
        airBefore = converterHeldAir_->airHz;
    }
    converterHeldAir_.reset();
    // STORED EVEN WHEN OFF, so switching back on finds the LO the user typed
    // (sanitiseConverter keeps a valid LO whatever the mode).
    converters_[key] = cc::sanitiseConverter(s);
    const cc::ConverterSetting eff = converterForKey(key);
    pipeline_.setConverter(eff);
    tuneMismatchNote_.clear();

    // THE AIR FREQUENCY STAYS; THE RADIO FOLLOWS. On every change - on, off,
    // mode, LO, inversion - the station the user is listening to is kept (the
    // air centre, and with it the VFO's station) and the radio is retuned to
    // what the new setting makes of it. SYMMETRIC, which the rule before it
    // was not: that one relabelled unless the result fell below 0 Hz, so an LO
    // typo (125 -> 1250 MHz) moved the radio to 1250.0172 MHz while correcting
    // it (1250 -> 125) only relabelled the counter to 1125.0172 MHz, and the
    // station was gone; the quick LO keys moved the radio one way round and
    // relabelled the other (second review, probe P2).
    //
    // ONLY WHEN THE RADIO CAN GO THERE: above 0 Hz at the radio, and inside
    // the range the radio publishes. Otherwise it stays where it is, the
    // counter relabels to what it now hears, and the note says what the radio
    // reaches (converterTuneNote, or the plain sentence with the converter
    // off). No value in airBefore is a radio that was never tuned: there is
    // no station to keep, and the relabel is all there is.
    //
    // AN I/Q FILE IS NOT A RADIO. Its frequency is where the recording was
    // made; a converter set on it is there to relabel a recording made at the
    // radio's frequency, so a file always relabels. NOR IS A SOUND CARD: it
    // has no tuner to move (a real-mode card sits at rate/4, an I/Q card
    // where its external receiver is), so a converter in front of one only
    // relabels what it hears - asking it to retune would be refused and
    // said as "out of reach".
    if (airBefore.has_value() && sourceKind_ != "file" && sourceKind_ != "soundcard") {
        const double airHz = *airBefore;
        const double radioHz = cc::radioFromAir(eff, airHz);
        double rLo = 0.0;
        double rHi = 0.0;
        const bool hasRange =
            device_ != nullptr && device_->frequencyRangeHz(rLo, rHi) && rHi > rLo;
        const bool reachable = std::isfinite(radioHz) && radioHz > 0.0 &&
                               (!hasRange || (radioHz >= rLo && radioHz <= rHi));
        if (reachable) {
            retuneCoalescer_.clearPending();
            applyRetuneNow(airHz);
        } else {
            converterHeldAir_ = ConverterHeldAir{key, airHz, radioNowHz};
            tuneMismatchNote_ =
                cc::converterActive(eff)
                    ? converterTuneNote(airHz, /*refused=*/true, 0.0, /*isPluginPreset=*/false)
                    : cascade::gui::tuneRefusedMessage(airHz, hasRange, rLo, rHi,
                                                       /*isPluginPreset=*/false);
            // Where it fell, never the frequency (PRIVACY.md).
            cascade::core::diagLogf("source: converter change: the %s cannot follow the air "
                                    "frequency (%s); it stays where it was",
                                    pipeline_.activeSource().name(),
                                    !(radioHz > 0.0) ? "0 Hz or below at the radio"
                                    : radioHz < rLo   ? "below its range"
                                                      : "above its range");
        }
    }
    // A new frequency as far as everything downstream is concerned (a relabel
    // is one too; applyRetuneNow already told them when the radio moved).
    pipeline_.resetRds();
    pluginRunner_.retune(pipeline_.activeSource().centerFrequencyHz());
    host_->onConverterChanged(false);
    // Which way it was set, never a frequency (PRIVACY.md: what somebody
    // tunes to stays out of reports - an LO says which band they listen to).
    cascade::core::diagLogf("source: converter %s%s for the %s", cc::converterModeKey(eff.mode),
                            eff.inverted && cc::converterActive(eff) ? " (inverted)" : "",
                            sourceKind_.c_str());
}

}  // namespace cascade::engine
