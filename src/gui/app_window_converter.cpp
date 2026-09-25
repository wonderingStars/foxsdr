// app_window_converter.cpp - the up- or down-converter in front of the radio
// (0.99.36): the Source section's Converter rows, the per-radio memory behind
// them, and the sentences that say what a converter means for a tune. AppWindow
// members, kept out of app_window.cpp because they are one subject.
//
// THE OWNER APPROVED THIS FROM A BETA TESTER'S REQUEST. He receives VLF - SAQ
// Grimeton on 17.2 kHz - through home-built up-converters with 2 MHz, 100 MHz
// and 125 MHz local oscillators; the common Ham-It-Up style converters use
// 125 MHz. Until now he had to tune the radio to 125.0172 MHz and do the
// subtraction himself, and every preset, bookmark, band plan entry and
// decoder frequency in the application was 125 MHz away from what he heard.
//
// WHAT THIS FILE DOES NOT DO IS CONVERT. core/freq_converter.hpp holds the
// arithmetic and the pipeline applies it (Pipeline::activeSource() speaks the
// AIR frequency; see source/converter_view.hpp). This file decides WHICH
// setting is in force - the installed radio's own, remembered under
// core::converterRadioKey - and lets the user change it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/theme.hpp"
#include "gui/tune_control.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cc = cascade::core;

namespace {

// The three local oscillators the tester uses, 125 MHz first because it is
// the common Ham-It-Up's.
constexpr double kQuickLoHz[] = {125.0e6, 100.0e6, 2.0e6};

// The LO a mode switched on with none remembered starts from.
constexpr double kDefaultLoHz = 125.0e6;

// Sentences joined the way the language writes them: a space after an ASCII
// full stop, none after a CJK one ("。"), which is not followed by a space.
void appendSentence(std::string& msg, const std::string& next) {
    if (next.empty()) { return; }
    if (!msg.empty() && static_cast<unsigned char>(msg.back()) < 0x80u) { msg += ' '; }
    msg += next;
}

}  // namespace

std::string AppWindow::resolveConverterKey(const std::string& radioKey) const {
    const auto it = converterKeyAlias_.find(radioKey);
    return it == converterKeyAlias_.end() ? radioKey : it->second;
}

std::string AppWindow::converterRadioKeyNow() const {
    return resolveConverterKey(cc::converterRadioKey(sourceKind_, deviceArgs_));
}

cc::ConverterSetting AppWindow::converterForKey(const std::string& radioKey) const {
    return cc::converterFor(converters_, resolveConverterKey(radioKey));
}

void AppWindow::noteConverterFallback(const std::string& nativeKey,
                                      const std::string& fallbackKey) {
    // A Soapy key with a converter of its own keeps it: the user set that one
    // for this very way of reaching the dongle.
    if (nativeKey.empty() || nativeKey == fallbackKey || converters_.count(fallbackKey) != 0) {
        return;
    }
    converterKeyAlias_[fallbackKey] = nativeKey;
    const cc::ConverterSetting s = converterForKey(fallbackKey);
    // Which way it is set, never a frequency (see changeConverter).
    cascade::core::diagLogf("source: the SoapySDR fallback keeps this radio's converter (%s%s)",
                            cc::converterModeKey(cc::converterActive(s) ? s.mode
                                                                        : cc::ConverterMode::Off),
                            s.inverted && cc::converterActive(s) ? ", inverted" : "");
}

std::string AppWindow::converterAliasNote() {
    const std::string raw = cc::converterRadioKey(sourceKind_, deviceArgs_);
    if (converterKeyAlias_.count(raw) == 0) { return {}; }
    if (!cc::converterActive(pipeline_.converter())) { return {}; }
    return tr("Opened through SoapySDR because the native driver refused this radio - the "
              "converter set for it still applies.");
}

std::optional<double> AppWindow::carriedAirCentre() {
    // AT THE RADIO, because that is where "no frequency" shows: a device that
    // was never tuned reads 0 Hz. The AIR figure is what is carried, and it
    // may be below 0 Hz (a VLF station with the VFO parked up, through an
    // up-converter) - which is a frequency, not the absence of one.
    if (!(pipeline_.rawSource().centerFrequencyHz() > 0.0)) { return std::nullopt; }
    return pipeline_.activeSource().centerFrequencyHz();
}

void AppWindow::applyConverterForSource() {
    pipeline_.setConverter(converterForKey(converterRadioKeyNow()));
    // The LO field re-seeds from the radio now installed.
    converterLoSeededFor_.clear();
    converterLoBad_ = false;
}

double AppWindow::radioHzForSource(const std::string& kind, const std::string& args,
                                   double airHz) const {
    return cc::radioFromAir(converterForKey(cc::converterRadioKey(kind, args)), airHz);
}

std::string AppWindow::converterName(const cc::ConverterSetting& s) const {
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

std::string AppWindow::converterStatusLine(bool shortForm) {
    const cc::ConverterSetting conv = pipeline_.converter();
    if (!cc::converterActive(conv)) { return {}; }
    // THE RADIO'S OWN READBACK, not a sum: this line exists so that what the
    // radio reports (its lights, another program, its own display) is on the
    // screen beside the air frequency the counter shows. The short form names
    // the converter by its LO alone, for a column too narrow for the full
    // name (an inverting converter's is the longest).
    const double radioHz = pipeline_.rawSource().centerFrequencyHz();
    const std::string name =
        shortForm ? cc::converterHzText(conv.loHz) + " LO" : converterName(conv);
    std::string out;
    cascade::core::formatUtf8(out, tr("via %s - radio at %s"), name.c_str(),
                              cc::converterHzText(radioHz).c_str());
    return out;
}

std::string AppWindow::converterTuneNote(double requestAirHz, bool refused, double answeredAirHz,
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
        // The same rule as the plain sentence (gui/tune_control.hpp,
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

void AppWindow::changeConverter(const cc::ConverterSetting& s) {
    const std::string key = converterRadioKeyNow();
    const std::optional<double> airBefore = carriedAirCentre();
    // STORED EVEN WHEN OFF, so switching back on finds the LO the user typed
    // (sanitiseConverter keeps a valid LO whatever the mode).
    converters_[key] = cc::sanitiseConverter(s);
    const cc::ConverterSetting eff = converterForKey(key);
    pipeline_.setConverter(eff);

    // THE RADIO STAYS WHERE IT IS; the counter relabels. A user who switches
    // a converter on is usually already tuned to its output (the tester had
    // his dongle on 125.0172 MHz to hear SAQ), and the counter now simply says
    // 17.2 kHz. The exception is a relabel whose TUNED frequency lands below
    // 0 Hz - a dongle left on 100 MHz behind a 125 MHz up-converter - where
    // there is nothing to show: then the AIR frequency is kept and the radio
    // moves, through the ordinary tune path so a refusal is reported the
    // ordinary way. Judged on the tuned frequency, not the band centre: a
    // centre below 0 Hz on the air with the VFO parked above a VLF station
    // (the dongle on 124.7164 MHz, the VFO 300 kHz up, hearing 16.4 kHz) is
    // exactly the relabel the user wants.
    const double tunedNow = pipeline_.activeSource().centerFrequencyHz() + pipeline_.vfoOffsetHz();
    if (cc::converterActive(eff) && !(tunedNow >= 0.0) && airBefore.has_value()) {
        retuneCoalescer_.clearPending();
        applyRetuneNow(*airBefore);
    }
    // A new frequency as far as everything downstream is concerned.
    pipeline_.resetRds();
    pluginRunner_.retune(pipeline_.activeSource().centerFrequencyHz());
    tuneMismatchNote_.clear();
    converterLoSeededFor_.clear();
    // Which way it was set, never a frequency (PRIVACY.md: what somebody
    // tunes to stays out of reports - an LO says which band they listen to).
    cascade::core::diagLogf("source: converter %s%s for the %s", cc::converterModeKey(eff.mode),
                            eff.inverted && cc::converterActive(eff) ? " (inverted)" : "",
                            sourceKind_.c_str());
}

void AppWindow::drawConverterControls() {
    const std::string key = converterRadioKeyNow();
    const auto stored = converters_.find(key);
    const cc::ConverterSetting mine =
        stored == converters_.end() ? cc::ConverterSetting{} : stored->second;
    const cc::ConverterSetting live = pipeline_.converter();

    ImGui::SeparatorText(tr("Converter"));
    // Not while a radio is being opened: the setting would land on whichever
    // radio happened to be installed at that instant.
    ImGui::BeginDisabled(deviceOpenPending_);

    // --- the mode ----------------------------------------------------------------
    const char* modeNames[] = {tr("Off"), tr("Up-converter"), tr("Down-converter")};
    int mode = static_cast<int>(mine.mode);
    if (mode < 0 || mode > 2) { mode = 0; }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##converter_mode", &mode, modeNames, 3)) {
        cc::ConverterSetting next = mine;
        next.mode = static_cast<cc::ConverterMode>(mode);
        if (next.mode != cc::ConverterMode::Off && !cc::converterLoValid(next.loHz)) {
            next.loHz = kDefaultLoHz;
        }
        changeConverter(next);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          tr("An up- or down-converter between the antenna and this radio. FoxSDR "
                             "then shows and tunes the frequency on the air, and tells the radio "
                             "the converted one. Remembered for this radio only."));
    }
    // The SoapySDR fallback for a dongle the native driver refused: the same
    // radio under another key, and the converter set for it still in force.
    if (const std::string note = converterAliasNote(); !note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }

    if (mine.mode != cc::ConverterMode::Off) {
        // --- the local oscillator -------------------------------------------------
        // Seeded from the remembered LO whenever the radio or the LO changes
        // under it; while the user types, what they type stays.
        const std::string seed = key + "\x1f" + cc::converterHzText(mine.loHz);
        if (converterLoSeededFor_ != seed) {
            std::snprintf(converterLoBuf_, sizeof(converterLoBuf_), "%s",
                          cc::converterLoValid(mine.loHz) ? cc::converterHzText(mine.loHz).c_str()
                                                          : "");
            converterLoSeededFor_ = seed;
            converterLoBad_ = false;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Local oscillator"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputText("##converter_lo", converterLoBuf_, sizeof(converterLoBuf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Type it in Hz, kHz or MHz - 125 MHz, 125000 kHz and "
                                       "125000000 Hz are the same. A number with no unit is MHz."));
        }
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            double lo = 0.0;
            if (cc::parseConverterLoHz(converterLoBuf_, lo)) {
                converterLoBad_ = false;
                if (lo != mine.loHz) {
                    cc::ConverterSetting next = mine;
                    next.loHz = lo;
                    changeConverter(next);
                } else {
                    converterLoSeededFor_.clear();  // tidy the text back to its canonical form
                }
            } else {
                converterLoBad_ = true;
            }
        }
        // The three the tester's converters use, as one-press choices.
        for (int i = 0; i < 3; ++i) {
            if (i > 0) { ImGui::SameLine(); }
            const std::string label = cc::converterHzText(kQuickLoHz[i]) + "##converter_quick" +
                                      std::to_string(i);
            const bool on = mine.loHz == kQuickLoHz[i];
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::SmallButton(label.c_str()) && !on) {
                cc::ConverterSetting next = mine;
                next.loHz = kQuickLoHz[i];
                changeConverter(next);
            }
            if (on) { ImGui::PopStyleColor(); }
        }

        // --- inversion ------------------------------------------------------------
        bool inv = mine.inverted;
        if (ImGui::Checkbox(trId("Inverts the spectrum (LO above the signal)"), &inv)) {
            cc::ConverterSetting next = mine;
            next.inverted = inv;
            changeConverter(next);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s", tr("Tick this when the converter's oscillator sits ABOVE the signal (a "
                         "high-side LO): the radio is then tuned to the oscillator minus the "
                         "frequency, and the band arrives back to front. FoxSDR turns it the "
                         "right way round. Most up-converters, a Ham-It-Up included, do not."));
        }

        if (converterLoBad_) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
            std::string msg;
            cascade::core::formatUtf8(msg, tr("could not read frequency \"%s\""), converterLoBuf_);
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
        }

        // --- what it means right now ---------------------------------------------
        if (cc::converterActive(live)) {
            // THE STATION, not the band centre: with the VFO parked off-centre
            // the centre can sit below 0 Hz on the air (a 16.4 kHz station
            // with the VFO 300 kHz up), which is true and says nothing useful.
            // The status column carries the radio's own centre readback.
            const double airHz = currentAbsoluteHz();
            const double radioHz = cc::radioFromAir(live, airHz);
            std::string line;
            cascade::core::formatUtf8(line, tr("Listening on %s on the air - %s at the radio."),
                                      cc::converterHzText(airHz).c_str(),
                                      cc::converterHzText(radioHz).c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndDisabled();
}

}  // namespace cascade::gui
