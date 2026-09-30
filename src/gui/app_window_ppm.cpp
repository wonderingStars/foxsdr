// app_window_ppm.cpp - the crystal correction ("PPM frequency correction",
// 0.99.56): the Source section's switch and value, the per-radio memory behind
// them, and applying the value to the open radio. AppWindow members, kept out
// of app_window.cpp because they are one subject.
//
// THE OWNER ASKED FOR IT: "add a PPM frequency correction function that you
// can toggle on in settings". So it is a section of its own under Settings
// (the SYSTEM bank, drawPpmSection), and the same controls sit in the Source
// section beside the converter (drawPpmControls), because that is where every
// setting that belongs to ONE radio is set - the switch is global, the value
// is that radio's.
//
// WHAT THIS FILE DOES NOT DO IS ARITHMETIC. core/ppm_correction.hpp holds the
// conversion and the rules; the source view (source/converter_view.hpp)
// applies the retune fallback; a radio that corrects its own crystal does it
// itself (IqSource::setFrequencyCorrectionPpm). This file decides which value
// is in force - the open radio's own, under core::ppmRadioKey - and how.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cfloat>
#include <cmath>
#include <string>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/ppm_correction.hpp"
#include "core/utf8_text.hpp"
#include "gui/bench_rail.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cc = cascade::core;

std::string AppWindow::ppmRadioKeyNow() const { return cc::ppmRadioKey(sourceKind_, deviceArgs_); }

double AppWindow::ppmForKey(const std::string& key) const {
    return cc::ppmFor(ppmCorrectionOn_, ppmValues_, key);
}

double AppWindow::ppmForPatchDevice(const std::string& deviceKey) const {
    namespace pc = cascade::core::patch;
    return ppmForKey(cc::ppmRadioKey(pc::deviceDriver(deviceKey), pc::deviceArgs(deviceKey)));
}

cc::PpmMethod AppWindow::ppmMethodNow() {
    return cc::ppmMethodFor(sourceKind_, pipeline_.rawSource().hasFrequencyCorrection());
}

void AppWindow::applyPpmForSource() {
    // A RADIO OPENS WITH NO CORRECTION OF ITS OWN, and setSource has already
    // put the pipeline's view back to none. Nothing below runs - nothing at
    // all is sent to the radio - while the switch is off or this radio has no
    // value: off is exactly the behaviour before this existed.
    ppmRadioApplied_ = 0.0;
    ppmError_.clear();
    ppmEditSeededFor_.clear();
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

double AppWindow::ppmPreTellHz(const std::string& kind, const std::string& args,
                               bool radioCorrects, double radioHz) {
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

void AppWindow::changePpm(bool on, double ppm) {
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
    ppmEditSeededFor_.clear();
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

std::string AppWindow::ppmCardLine() {
    if (!ppmCorrectionOn_) { return {}; }
    const cc::PpmMethod method = ppmMethodNow();
    if (method == cc::PpmMethod::NotApplicable) { return {}; }
    double shown = ppmForKey(ppmRadioKeyNow());
    // What the radio really took: a whole-ppm register gets the rounded value.
    if (method == cc::PpmMethod::InRadio && cc::ppmRadioTakesWholePpm(sourceKind_)) {
        shown = static_cast<double>(cc::ppmWholeForRadio(shown));
    }
    std::string out;
    cascade::core::formatUtf8(out, tr("PPM %+.1f"), shown);
    return out;
}

std::string AppWindow::ppmDiagText() {
    if (!ppmCorrectionOn_) { return "off"; }
    const cc::PpmMethod method = ppmMethodNow();
    if (method == cc::PpmMethod::NotApplicable) { return "not applicable"; }
    const std::string value = cc::ppmText(ppmForKey(ppmRadioKeyNow()));
    if (method == cc::PpmMethod::InRadio) {
        return value + (ppmError_.empty() ? " in the radio" : " in the radio (refused)");
    }
    return value + " by retuning";
}

void AppWindow::drawPpmControls() {
    ImGui::SeparatorText(tr("Frequency correction"));
    drawPpmBody();
}

void AppWindow::drawPpmSection() {
    // THE SAME CONTROLS UNDER SETTINGS (the SYSTEM bank - the owner asked for
    // a correction "you can toggle on in settings"), with the value in force
    // on the row's chip. The Source section carries them too, beside the
    // converter, because that is where a radio's own settings are set.
    const std::string card = ppmCardLine();
    const char* chip = !ppmCorrectionOn_ ? tr("OFF") : (card.empty() ? tr("ON") : card.c_str());
    if (!railSection(trId("Frequency correction###ppm"), false, chip, cascade::gui::theme::kPhosphor,
                     ppmCorrectionOn_)) {
        return;
    }
    drawPpmBody();
}

void AppWindow::drawPpmBody() {
    // Not while a radio is being opened: the value would land on whichever
    // radio happened to be installed at that instant.
    ImGui::BeginDisabled(deviceOpenPending_);
    const std::string key = ppmRadioKeyNow();
    const double stored = cc::ppmFor(true, ppmValues_, key);
    bool on = ppmCorrectionOn_;
    if (ImGui::Checkbox(trId("PPM frequency correction"), &on)) { changePpm(on, stored); }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s", tr("Corrects the error of this radio's crystal, in parts per million. Positive "
                     "when stations show up below their real frequency. Tune to a signal whose "
                     "frequency you know exactly and change the value until it sits where it "
                     "should. The switch is for every radio; the value is remembered for each "
                     "radio."));
    }
    if (ppmCorrectionOn_) {
        const cc::PpmMethod method = ppmMethodNow();
        if (method == cc::PpmMethod::NotApplicable) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", tr("Not used for the signal generator, a sound card or an I/Q "
                                        "file - none has a crystal of its own to correct."));
            ImGui::PopStyleColor();
        } else {
            // WHICH RADIO the value is for - the serial is what tells two
            // identical dongles apart. On screen only; never logged.
            const std::string model =
                deviceModel_.empty() ? std::string(pipeline_.activeSource().name()) : deviceModel_;
            const std::string serial = cascade::source::argValue(deviceArgs_, "serial");
            std::string whose;
            if (serial.empty()) {
                cascade::core::formatUtf8(whose, tr("For %s"), model.c_str());
            } else {
                cascade::core::formatUtf8(whose, tr("For %s, serial %s"), model.c_str(),
                                          serial.c_str());
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", whose.c_str());
            ImGui::PopStyleColor();

            // Seeded from the remembered value whenever the radio or the
            // value changes under it; while the user types, what they type
            // stays.
            const std::string seed = key + "\x1f" + cc::ppmText(stored);
            if (ppmEditSeededFor_ != seed) {
                ppmEdit_ = stored;
                ppmEditSeededFor_ = seed;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool stepped = ImGui::InputDouble("##ppm_value", &ppmEdit_, cc::kPpmStep, 1.0,
                                                    "%+.1f ppm", ImGuiInputTextFlags_EnterReturnsTrue);
            if (stepped || ImGui::IsItemDeactivatedAfterEdit()) {
                const double v = cc::sanitisePpm(ppmEdit_);
                ppmEdit_ = v;
                if (v != stored) {
                    changePpm(true, v);
                } else {
                    ppmEditSeededFor_.clear();  // tidy a clamped entry back to its value
                }
            }

            // HOW it is applied, which is not the same promise: a radio that
            // corrects itself corrects its sample rate too; the retune fixes
            // the centre frequency and nothing else.
            std::string how;
            if (method == cc::PpmMethod::InRadio) {
                if (cc::ppmRadioTakesWholePpm(sourceKind_)) {
                    cascade::core::formatUtf8(how, tr("Corrected in the radio itself, in whole ppm: %+d."),
                                              cc::ppmWholeForRadio(stored));
                } else {
                    how = tr("Corrected in the radio itself.");
                }
            } else {
                how = tr("Corrected by retuning - the centre frequency only; the sample rate keeps "
                         "its error.");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", how.c_str());
            ImGui::PopStyleColor();
            if (!ppmError_.empty()) {
                std::string msg;
                cascade::core::formatUtf8(msg, tr("The radio refused the correction: %s"),
                                          ppmError_.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
                ImGui::TextWrapped("%s", msg.c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndDisabled();
}

}  // namespace cascade::gui
