// app_window_display.cpp - WHAT THE SPECTRUM AND THE WATERFALL SHOW, AND HOW THE PASSBAND IS MOVED ALONG THEM (stub campaign B11, MARKERS-B and DISPLAY-RANGE; master
// 0.99.57, 0.99.76, 0.99.82 and 0.99.83). AppWindow members, kept out of app_window.cpp because they are one subject (and because that file is at the object-file limit).
//
// THE TRACE MODES (a user's request, 2026-09-30: "if you right click on the spectrum display ... give the user the option to select different display modes ... normal,
// peak, average ... have this trace drawn in bold with the normal trace shown as before ... reset the trace ... set the length of the average, maybe in milliseconds").
// Normal, Peak hold, Average over a length; one setting for every spectrum, the held trace drawn in bold over the live one, a label at the left under the header.
//
// THE DISPLAY RANGE (a French user on 0.99.75, RTL-SDR V4: "the spectrum and the waterfall are very milky, one cannot see the signals"; the owner: "allow the users to
// set it themselves to what they prefer by right clicking on it", and "add a tool tip when the mouse is over it for 4 secs saying you can right click to change it").
// Range: Automatic or Manual, a Floor and a Ceiling, Reset: the block that ends the spectrum's menu and the waterfall's. core/display_range.hpp has the rules (the ceiling
// stays 10 dB above the floor and THE END THAT MOVED GIVES WAY; Automatic keeps the pair; Reset forgets it).
//
// THE PASSBAND'S SLIDE (a Linux user, 0.99.82): Shift + wheel over the spectrum or the waterfall moves the tuned frequency along the band by the mode's raster a notch.
// gui/passband_slide.hpp has the arithmetic.
//
// THE TYPED FREQUENCY'S MODE (field report a09fe097, 0.99.83): typing a frequency in another band adopts that band's conventional mode.
//
// WHERE THE STATE LIVES. In master the window held all of it next to the pipeline and wrote its picture's floats. Here the ENGINE holds the dB window IN FORCE (dbMin_/dbMax_,
// the state's dbMin/dbMax, what the spectrum and the waterfall are drawn through) and takes it by FOXAPI_OP_SET_DISPLAY_RANGE; WHICH WINDOW THAT IS - Automatic's, or the
// person's pair - is the person's choice about what they see, kept here (displayRange_) and saved here, as the trace mode is. NO NEW OP, LIST OR FACT: the door did not move.
// The one engine-side change of the range that is not the window's - the web remote's - is seen in the state and taken as a choice of Manual, as master's onDisplayRange did
// (followDisplayRange).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

#include "core/band_mode.hpp"
#include "core/diag_log.hpp"
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/fonts.hpp"
#include "gui/passband_slide.hpp"
#include "gui/rail_reads.hpp"
#include "gui/scope_memory.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cmd = cascade::core::cmd;

// --- the spectrum's trace mode ------------------------------------------------------------------------------------------------------------------

void AppWindow::setSpectrumTraceMode(cascade::core::TraceMode m) {
    spectrumTraceMode_ = m;
    spectrumHold_.setMode(m);
    for (auto& [id, hold] : patchHolds_) {
        (void)id;
        hold.setMode(m);
    }
}

void AppWindow::setSpectrumAverageMs(double ms) {
    spectrumAverageMs_ = cascade::core::clampTraceAverageMs(ms);
    spectrumHold_.setAverageMs(spectrumAverageMs_);
    for (auto& [id, hold] : patchHolds_) {
        (void)id;
        hold.setAverageMs(spectrumAverageMs_);
    }
}

void AppWindow::resetSpectrumTraces() {
    spectrumHold_.reset();
    for (auto& [id, hold] : patchHolds_) {
        (void)id;
        hold.reset();
    }
}

void AppWindow::feedSpectrumHold() {
    // A NEW FRAME, not a new GUI frame: readSpectrum takes at most one a frame and says nothing when the stream has not moved, and a hold fed twice with the same frame
    // would count it twice in an average.
    if (spectrumTraceMode_ == cascade::core::TraceMode::Normal || lastFrame_.dbBins.empty() || lastFrame_.seq == spectrumHoldSeq_) { return; }
    spectrumHoldSeq_ = lastFrame_.seq;
    // The axis: which frequency each bin is. A retune, a new sample rate or a new FFT size starts the held trace again (core/trace_hold.hpp).
    const auto bitsOf = [](double v) {
        std::uint64_t b = 0;
        std::memcpy(&b, &v, sizeof b);
        return b;
    };
    const std::uint64_t key = scopeMemoryKey(static_cast<std::uint64_t>(lastFrame_.dbBins.size()), bitsOf(uiState_.centreHz), bitsOf(tuningLists().inputRateHz));
    spectrumHold_.push(lastFrame_.dbBins.data(), static_cast<int>(lastFrame_.dbBins.size()), uiClockS_(), key);
}

unsigned int AppWindow::traceColour() const {
    // Hot for a peak, ivory for an average: neither is the live trace's phosphor, and the two modes are told apart at a glance.
    return spectrumTraceMode_ == cascade::core::TraceMode::Peak ? cascade::gui::theme::kAlarmHot : cascade::gui::theme::kIvory;
}

void AppWindow::drawTraceModeLabel(float x0, float y0, float scale) {
    if (spectrumTraceMode_ == cascade::core::TraceMode::Normal) { return; }
    std::string label;
    if (spectrumTraceMode_ == cascade::core::TraceMode::Peak) {
        label = tr("PEAK HOLD");
    } else {
        char len[32];
        if (spectrumAverageMs_ < 1000.0) {
            std::snprintf(len, sizeof len, "%.0f ms", spectrumAverageMs_);
        } else {
            std::snprintf(len, sizeof len, "%g s", spectrumAverageMs_ / 1000.0);
        }
        cascade::core::formatUtf8(label, tr("AVERAGE %s"), len);
    }
    ImFont* font = cascade::gui::fonts::ui();
    const float px = ImGui::GetFontSize() * 0.8f;
    // At the LEFT, straight under the header: the right-hand corner carries the three-line passband reading, and the first rendered check put this label across its
    // last line. Nothing else is drawn in this row - the bookmark names start one line lower - and it ends well short of the centre.
    const ImVec2 at(x0 + 6.0f * scale, y0 + 2.0f * scale);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(font, px, at, traceColour(), label.c_str());
    cascade::gui::census::note("trc:label:", cascade::core::traceModeName(spectrumTraceMode_));
}

void AppWindow::drawTraceModeMenu() {
    static constexpr const char* kMenuId = "##trace_mode_menu";
    if (traceMenuPending_) {
        traceMenuPending_ = false;
        ImGui::OpenPopup(kMenuId);
    }
    if (!ImGui::BeginPopup(kMenuId)) { return; }
    cascade::gui::census::note("trc:menu");
    // Each item's rectangle, for tests/test_spectrum_trace_app to aim at.
    const auto itemRect = [](const char* name) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        cascade::gui::census::rect(name, a.x, a.y, b.x, b.y);
    };
    using cascade::core::TraceMode;
    if (ImGui::MenuItem(tr("Normal"), nullptr, spectrumTraceMode_ == TraceMode::Normal)) { setSpectrumTraceMode(TraceMode::Normal); }
    itemRect("trc:menu:normal");
    if (ImGui::MenuItem(tr("Peak hold"), nullptr, spectrumTraceMode_ == TraceMode::Peak)) { setSpectrumTraceMode(TraceMode::Peak); }
    itemRect("trc:menu:peak");
    if (ImGui::MenuItem(tr("Average"), nullptr, spectrumTraceMode_ == TraceMode::Average)) { setSpectrumTraceMode(TraceMode::Average); }
    itemRect("trc:menu:average");
    ImGui::Separator();
    // Starts the peak or the average over, on every spectrum.
    if (ImGui::MenuItem(tr("Reset trace"), nullptr, false, spectrumTraceMode_ != TraceMode::Normal)) { resetSpectrumTraces(); }
    itemRect("trc:menu:reset");
    if (ImGui::BeginMenu(tr("Average length"))) {
        static constexpr int kPresetsMs[] = {100, 250, 500, 1000, 2000, 5000, 10000};
        for (int ms : kPresetsMs) {
            char label[32];
            if (ms < 1000) {
                std::snprintf(label, sizeof label, "%d ms", ms);
            } else {
                std::snprintf(label, sizeof label, "%d s", ms / 1000);
            }
            if (ImGui::MenuItem(label, nullptr, std::fabs(spectrumAverageMs_ - ms) < 0.5)) { setSpectrumAverageMs(static_cast<double>(ms)); }
            cascade::gui::census::rect("trc:menu:len:", ms, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        }
        ImGui::Separator();
        // Any length between, on a logarithmic slider.
        int ms = static_cast<int>(std::lround(spectrumAverageMs_));
        ImGui::SetNextItemWidth(cascade::gui::uiscale::px(180.0f));
        if (ImGui::SliderInt("##trc_avg_ms", &ms, static_cast<int>(cascade::core::kTraceAverageMinMs), static_cast<int>(cascade::core::kTraceAverageMaxMs), "%d ms",
                             ImGuiSliderFlags_Logarithmic)) {
            setSpectrumAverageMs(static_cast<double>(ms));
        }
        ImGui::EndMenu();
    }
    itemRect("trc:menu:length");
    // The display range, last: a right-click on the spectrum finds it here and a right-click on the waterfall finds it at the foot of the marker menu
    // (drawMarkerMenu), the same block.
    drawDisplayMenuBlock();
    ImGui::EndPopup();
}

// --- the display range ----------------------------------------------------------------------------------------------------------------------------

void AppWindow::sendDisplayRange() {
    // The window in force, as one command: the engine takes both ends at once (and would have the ceiling at least 10 dB above the floor, which clampManualRange has
    // already made true), so there is no order of two sliders' commands to get wrong.
    const cascade::core::DbPair w = displayRange_.inForce();
    displaySent_.pending = true;
    displaySent_.pair = w;
    displaySent_.atS = uiClockS_();
    uiSubmit(cmd::makeNum(FOXAPI_OP_SET_DISPLAY_RANGE, static_cast<double>(w.floorDb), static_cast<double>(w.ceilingDb)));
}

void AppWindow::followDisplayRange() {
    // Once a frame, after the state was read (refreshUiState: no ImGui here). What the picture is drawn through is the STATE's pair; this keeps the person's choice and
    // the state's from disagreeing for longer than a command takes to land.
    const cascade::core::DbPair st{static_cast<float>(uiState_.dbMin), static_cast<float>(uiState_.dbMax)};
    const double nowS = uiClockS_();
    const bool changed = !displaySeenValid_ || !(st == displaySeen_);
    displaySeen_ = st;
    const bool seenBefore = displaySeenValid_;
    displaySeenValid_ = true;
    if (displaySent_.pending) {
        if (st == displaySent_.pair) {
            displaySent_.pending = false;   // landed
            return;
        }
        if (nowS - displaySent_.atS <= 2.0) { return; }   // not yet: a command lands at the engine's next pass
        // Two seconds and the state still shows something else: the engine would not take it as asked (a pair it clamped), or the session held it. The state is what
        // the picture shows, so it is the person's range from here (a figure that lands later is taken the same way).
        displaySent_.pending = false;
    } else if (!changed || !seenBefore) {
        if (!seenBefore && !(st == displayRange_.inForce())) { sendDisplayRange(); }   // first sight of a state that does not carry the window in force
        return;
    }
    if (st == displayRange_.inForce()) { return; }
    // A CHANGE nobody here asked for - the web remote's range, a plugin's - is a range chosen: Manual, as a slider is. restore() rather than setManual() so the request
    // keeps the exact pair it passed the engine's checks with (the web page's own bound is wider than the sliders').
    if (cascade::core::isUsablePair(st)) { displayRange_.restore(cascade::core::RangeMode::Manual, st); }
}

void AppWindow::restoreDisplayRange(const cascade::core::AppConfig& cfg) {
    // The display window (0.99.76): the pair is the user's floor and ceiling, kept whichever mode is saved; the mode says whether it is in force. The loader has
    // already read a file without a mode from the pair, and resolveRangeMode here is that same rule for a config handed in by a test or the remote control without
    // going through it. The engine was given the pair (Engine::applyConfig, as dbMin/dbMax); Automatic puts the standard window in force over it.
    const cascade::core::DbPair pair{cfg.dbMin, cfg.dbMax};
    displayRange_.restore(cascade::core::resolveRangeMode(cfg.waterfallRangeMode, pair), pair);
    sendDisplayRange();
}

void AppWindow::setRangeMode(cascade::core::RangeMode m) {
    displayRange_.setMode(m);
    sendDisplayRange();
}

void AppWindow::setManualRange(float floorDb, float ceilingDb, bool floorMoved) {
    displayRange_.setManual(floorDb, ceilingDb, floorMoved);
    sendDisplayRange();
}

void AppWindow::resetDisplay() {
    displayRange_.reset();
    sendDisplayRange();
    // The trace is the other half of what the spectrum looks like; a person who has been through Peak hold and Manual and wants the picture they started with wants
    // both back.
    setSpectrumTraceMode(cascade::core::TraceMode::Normal);
    resetSpectrumTraces();
}

void AppWindow::drawDisplayMenuBlock() {
    using cascade::core::RangeMode;
    // Each item's rectangle, for tests/test_display_menu_app to aim at.
    const auto itemRect = [](const char* name) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        cascade::gui::census::rect(name, a.x, a.y, b.x, b.y);
    };
    cascade::gui::census::note("wfm:menu");
    ImGui::SeparatorText(tr("Display"));

    // Two radio keys and not menu items: a MenuItem closes the popup on a click, and the next thing a person who has just chosen Manual does is drag the sliders under it.
    const bool manual = displayRange_.manual();
    if (ImGui::RadioButton(tr("Range: Automatic"), !manual)) { setRangeMode(RangeMode::Auto); }
    itemRect("wfm:menu:auto");
    bool explain = ImGui::IsItemHovered();
    if (ImGui::RadioButton(tr("Range: Manual"), manual)) { setRangeMode(RangeMode::Manual); }
    itemRect("wfm:menu:manual");
    explain = explain || ImGui::IsItemHovered();
    if (explain) {
        ImGui::SetTooltip("%s", tr("Automatic: the standard window; a patch Spectrum part fits\n"
                                   "itself to the signal. Manual: the floor and ceiling below,\n"
                                   "on the spectrum, the waterfall and every patch Spectrum part."));
    }

    // The sliders are the user's, so they are live only in Manual. They show the window in force (the standard one under Automatic) and the census records each one's
    // FRAME, not the label beside it, so a test can click a fraction of the way along it. The window in force is the person's choice (displayRange_), which moves on the
    // frame the slider does and not when the engine's state has caught up.
    ImGui::BeginDisabled(!manual);
    float floorEdit = displayRange_.inForce().floorDb;
    float ceilingEdit = displayRange_.inForce().ceilingDb;
    const float sliderW = cascade::gui::uiscale::px(180.0f);
    ImGui::SetNextItemWidth(sliderW);
    const bool floorChanged = ImGui::SliderFloat(tr("Floor"), &floorEdit, cascade::core::kFloorMinDb, cascade::core::kFloorMaxDb, "%.0f dB");
    cascade::gui::census::rect("wfm:menu:floor", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMin().x + sliderW, ImGui::GetItemRectMax().y);
    ImGui::SetNextItemWidth(sliderW);
    const bool ceilingChanged = ImGui::SliderFloat(tr("Ceiling"), &ceilingEdit, cascade::core::kCeilingMinDb, cascade::core::kCeilingMaxDb, "%.0f dB");
    cascade::gui::census::rect("wfm:menu:ceiling", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMin().x + sliderW, ImGui::GetItemRectMax().y);
    ImGui::EndDisabled();
    if (floorChanged || ceilingChanged) { setManualRange(floorEdit, ceilingEdit, floorChanged); }

    if (ImGui::MenuItem(tr("Reset display to defaults"))) { resetDisplay(); }
    itemRect("wfm:menu:reset");
}

void AppWindow::showDisplayTip(std::uint64_t region, bool over, const char* name) {
    // WHAT KEEPS THE TIP DOWN, besides the pointer leaving: a button held (a press, a drag on the band and a pan of the waterfall are each one), and the menu it
    // advertises, which is the moment it stops being news - opening this frame (the pending flags) or open already. ImGui already reports nothing under an open popup as
    // hovered, so for an open menu `over` and this agree; the rule is stated here as well so that it does not rest on ImGui's hover bookkeeping.
    const bool buttonDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const bool blocked = buttonDown || vfoDrag_ != VfoDrag::None || wfPanning_ || traceMenuPending_ || markerMenuPending_ || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    const ImGuiIO& io = ImGui::GetIO();
    if (!displayTip_.update(region, over, blocked, io.MousePos.x, io.MousePos.y, ImGui::GetTime())) { return; }
    // SetTooltip replaces any tooltip already set this frame (the panels' gesture tips, drawn above), so the one line is what is shown while the hand rests.
    ImGui::SetTooltip("%s", tr("Right-click to change the range and display"));
    displayTipShown_ = true;
    cascade::gui::census::note("wfm:tip:", name);
    // The rest the pointer had on the LAST frame the tip was up (the census keeps the last rect): a test that moves or presses afterwards compares the rest it ends with
    // against this one, the two measured by the same clock in the same run, instead of against a number of seconds.
    cascade::gui::census::rect("wfm:tip:lastshown", static_cast<float>(displayTip_.restS(ImGui::GetTime())), 0.0f, 0.0f, 0.0f);
}

// --- the passband's slide -------------------------------------------------------------------------------------------------------------------------

void AppWindow::slidePassband(double notches) {
    if (!std::isfinite(notches) || notches == 0.0) { return; }
    const double nowS = ImGui::GetTime();
    if (nowS - slideAtS_ > 0.5) { slideNotches_ = 0.0; }
    slideNotches_ += notches;
    const double whole = std::trunc(slideNotches_);
    if (whole == 0.0) {
        slideAtS_ = nowS;
        return;
    }
    slideNotches_ -= whole;

    // THE BASE IS WHAT THE LAST SLIDE ASKED FOR while the retune it caused has not reached the state (the engine paces a hardware retune, and a command lands at its next
    // pass): counting this notch from the readback would count the one before it again. Everything else counts from the state. The offset needs no such care - the VFO
    // drag's own rule keeps what was sent until the state shows it (tunedVfoOffsetHz) - but the centre is the slide's to keep.
    const double clockS = uiClockS_();
    if (slideAsk_.pending && (std::fabs(uiState_.centreHz - slideAsk_.centreHz) < 0.5 || clockS - slideAsk_.atS > 1.0)) { slideAsk_.pending = false; }
    const double centre = slideAsk_.pending ? slideAsk_.centreHz : uiState_.centreHz;
    const double offset = tunedVfoOffsetHz();
    const double tuned = centre + offset;
    const double rate = tuningLists().inputRateHz;
    const double step = kModeSnapHz[railModeIndex(uiState_.demodMode)];
    const double bandwidth = tunedBandwidthHz();
    const cascade::core::ConverterSetting conv = converterLists().live;
    PassbandSlide p = planPassbandSlide(tuned, centre, rate, bandwidth, step, whole, cascade::core::minTunedAirHz(conv, offset));
    if (p.retuneSource) {
        // The floor depends on the offset the passband ends up at, which is the end of its reach.
        const double floorHz = cascade::core::minTunedAirHz(conv, p.offsetHz);
        if (p.wantAbsHz < floorHz) { p = planPassbandSlide(tuned, centre, rate, bandwidth, step, whole, floorHz); }
    }
    slideAtS_ = nowS;
    if (!p.moved) { return; }
    // Inside the span the VFO moves and the source does not: the move a click makes. Past the end of the VFO's reach the passband goes to the end of it and the SOURCE
    // takes up the rest through the retune the counter uses (SET_FREQUENCY keeps the offset, so the offset is sent first).
    tuneLocalSent(tuneLocal_.offset, p.offsetHz, clockS);
    uiSubmit(cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, p.offsetHz));
    if (p.retuneSource) {
        uiSubmit(cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, p.wantAbsHz));
        slideAsk_.pending = true;
        slideAsk_.centreHz = p.centreHz;
        slideAsk_.offsetHz = p.offsetHz;
        slideAsk_.atS = clockS;
    }
    passbandFollowUntilS_ = nowS + kPassbandFollowS;
    cascade::gui::census::note("rx:slide");
}

// --- the typed frequency's mode ------------------------------------------------------------------------------------------------------------------

int AppWindow::modeForTypedFrequency(double typedHz) {
    // Both bands are read BEFORE the tune so `from` is the band the user is leaving, not the one just landed in. The whole rule, including the RAW hold and the
    // no-convention services, is core/band_mode.hpp's; this only reads the plan the engine holds (the rail's BAND_PLAN rows) and names the mode.
    const std::vector<BandRow>& bands = railLists().bands;
    const BandRow* to = bandRowAt(bands, typedHz);
    if (to == nullptr) { return 0; }
    const BandRow* from = bandRowAt(bands, std::max(0.0, uiState_.tunedHz));
    cascade::core::BandEntry toEntry;
    toEntry.startHz = to->startHz;
    toEntry.endHz = to->endHz;
    toEntry.name = to->name;
    toEntry.service = to->service;
    cascade::core::BandEntry fromEntry;
    const cascade::core::BandEntry* fromPtr = nullptr;
    if (from == to) {
        fromPtr = &toEntry;   // the rule compares the entries by identity: the same band is the same entry
    } else if (from != nullptr) {
        fromEntry.startHz = from->startHz;
        fromEntry.endHz = from->endHz;
        fromEntry.name = from->name;
        fromEntry.service = from->service;
        fromPtr = &fromEntry;
    }
    const char* follow = cascade::core::conventionalModeForBand(toEntry.service, typedHz);
    if (!cascade::core::modeFollowsTypedBandChange(fromPtr, &toEntry, kModeNames[railModeIndex(uiState_.demodMode)], follow)) { return 0; }
    for (int m = 0; m < 8; ++m) {
        if (std::strcmp(kModeNames[m], follow) == 0) {
            // The engine logs the mode and bandwidth the change moves to; this line says WHY it moved. The band's NAME only, never a frequency (core::scrubUploadLine).
            cascade::core::diagLogf("mode follows the band plan: %s", toEntry.name.c_str());
            return m + 1;
        }
    }
    return 0;
}

}  // namespace cascade::gui
