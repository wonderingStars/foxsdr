// app_window_patch_radios.cpp - the patch page's own radios and speaker
// outputs (0.99.17). AppWindow members, kept out of app_window.cpp because
// they are one subject: which devices the patch has open, where each
// speaker's sound goes, and handing the receiver's radio over and back.
//
// THE OWNER'S FOUR RULES, each of which is a function below:
//   - "add up to 5 sdrs ... each sdr input and out set in the patch panel":
//     every Radio node names its own device, rate and centre, and runs its
//     own core::patch::PatchRadio (patchReconcile);
//   - "only allow one device to be used in one panel at a time": a device on
//     one Radio is greyed out for every other (drawPatchRadioInspector), and
//     compile() refuses a patch that names one twice (DeviceTwice);
//   - "put all sound to mp3 or wav unless set to speakers or another device":
//     every speaker has an output, a WAV file by default (patchPublishSets);
//   - "when the patch panel is running unpopulate the sdr from the main sdr
//     software so it show signal generator": START on the page switches the
//     receiver to the generator and remembers its radio; STOP, ALL OFF or
//     closing the page opens it again (patchReconcile / patchStopAll).
// And since 0.99.18 each radio has its own ON/OFF switch (Node::on), which
// closes that radio alone while the rest of the patch keeps running.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "core/mp3_writer.hpp"
#include "core/patch_audio.hpp"
#include "core/patch_devices.hpp"
#include "engine/rate_follow_status.hpp"
#include "gui/scope_face.hpp"
#include "engine/soundcard_panel.hpp"
#include "gui/theme.hpp"
#include "source/siggen_source.hpp"
#include "source/soapy_source.hpp"

namespace pc = cascade::core::patch;

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// The rates offered for a patch radio. Every one either divides to a channel
// rate the plan accepts or is a rate a common radio runs at natively; a radio
// that refuses one keeps its own and says so on its face.
constexpr double kPatchRatesHz[] = {1.0e6,  1.024e6, 2.0e6, 2.048e6, 2.4e6,
                                    2.5e6,  3.0e6,   4.0e6, 6.0e6,   8.0e6, 10.0e6};

}  // namespace

bool AppWindow::setPatchRadioCentre(pc::Node& n, double airHz) {
    // THE RADIO decides, not the sign of the air figure: a node's centre is an
    // AIR frequency and may be carried in below 0 Hz through an up-converter,
    // so refusing everything <= 0 here made such a centre impossible to type
    // back in. What must hold is that the radio behind the node's converter
    // is told something above 0 Hz.
    const cascade::core::ConverterSetting conv = engine_.converterForKey(n.device);
    if (!cascade::core::radioCentreTakeable(conv, airHz)) {
        std::string msg;
        if (cascade::core::converterActive(conv)) {
            cascade::core::formatUtf8(
                msg, tr("%s is out of reach through the %s: the radio would have to tune to 0 Hz or below."),
                cascade::core::converterHzText(airHz).c_str(), engine_.converterName(conv).c_str());
        } else {
            cascade::core::formatUtf8(msg, tr("A radio cannot tune to %s - type a centre above 0 Hz."),
                                      cascade::core::converterHzText(airHz).c_str());
        }
        patchCentreNote_[n.id] = msg;
        return false;
    }
    n.freqHz = airHz;
    n.centreChosen = true;   // 0 Hz on the air is a centre too
    patchCentreNote_.erase(n.id);
    patchUi_.dirty = true;
    return true;
}

void AppWindow::drawPatchCentreNote(const pc::Node& n) {
    const auto it = patchCentreNote_.find(n.id);
    if (it == patchCentreNote_.end()) { return; }
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
    ImGui::TextWrapped("%s", it->second.c_str());
    ImGui::PopStyleColor();
}

std::string AppWindow::patchDefaultDeviceKey() const {
    const auto taken = [this](const std::string& key) {
        for (const pc::Node& n : engine_.patchGraph_.nodes()) {
            if (n.kind == pc::NodeKind::Radio && pc::sameDevice(n.device, key)) { return true; }
        }
        return false;
    };
    std::vector<std::string> candidates;
    if (engine_.patchMainKeep_.valid) {
        candidates.push_back(pc::makeDeviceKey(engine_.patchMainKeep_.kind, engine_.patchMainKeep_.args));
    } else if (engine_.device_ != nullptr) {
        candidates.push_back(pc::makeDeviceKey(engine_.sourceKind_, engine_.deviceArgs_));
    }
    for (const PatchDeviceChoice& c : engine_.patchDeviceChoices()) {
        if (!pc::isGeneratorKey(c.key)) { candidates.push_back(c.key); }
    }
    for (const std::string& k : candidates) {
        if (!taken(k)) { return k; }
    }
    return pc::kGeneratorKey;
}

void AppWindow::drawPatchTransport() {
    // THE SAME TRANSPORT AS THE MAIN PANEL (owner, 0.99.18: "a start button
    // like we have on the main panel so the user doesnt have to go between the
    // panels"). The dome is the receiver's own drawBenchStopButton, lettered
    // from the same bool it acts on, so it cannot say START while running.
    constexpr float kR = 24.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 centre(at.x + kR * 1.1f, at.y + kR * 1.1f);
    if (drawBenchStopButton(dl, centre, kR, engine_.patchRunning_)) {
        engine_.submitCommand(cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, engine_.patchRunning_ ? 0 : 1));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s", engine_.patchRunning_
                      ? tr("Stop the patch: every patch radio closes, recordings are\n"
                           "finished, and the receiver gets its radio back.")
                      : tr("Start the patch: every radio switched on opens. The receiver\n"
                           "hands its radio over and runs on the signal generator."));
    }

    // ALL OFF: larger, red, and always there. Every radio's switch goes off
    // and the patch stops.
    ImGui::SetCursorScreenPos(ImVec2(at.x + kR * 2.5f, at.y + kR * 0.25f));
    // A STOP control, so the stop button's roles (foxsdr-ui/1 stopBg and
    // stopText): today's rust and ivory exactly, and never Night Watch's
    // near-white "bad" as the face of a key that size.
    namespace th = cascade::gui::theme;
    ImGui::PushStyleColor(ImGuiCol_Button, th::toneHex(0xB8552F, 255, th::ink::StopBgBot));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th::toneHex(0xE07A4E, 255, th::ink::StopBgTop));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th::toneHex(0xE07A4E, 255, th::ink::StopBgTop));
    ImGui::PushStyleColor(ImGuiCol_Text, th::toneHex(0xEFE7D2, 255, th::ink::StopText));
    if (ImGui::Button(trId("ALL OFF###patchalloff"), ImVec2(150.0f, kR * 1.7f))) {
        engine_.submitCommand(cascade::core::cmd::make(FOXAPI_OP_PATCH_ALL_OFF));
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Switch every radio off and stop the patch. START switches\n"
                                   "them all on again."));
    }

    // What is running, in words.
    std::size_t radios = 0, on = 0;
    for (const pc::Node& n : engine_.patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Radio) { continue; }
        ++radios;
        if (n.on) { ++on; }
    }
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(
        ImVec2(ImGui::GetCursorScreenPos().x + 8.0f, at.y + kR * 1.1f - ImGui::GetTextLineHeight() * 0.5f));
    std::string line;
    if (engine_.patchRunning_) {
        // Singular and plural as whole keys: an English "s" handed in by %s
        // is a word no catalogue can translate.
        cascade::core::formatUtf8(line,
                      radios == 1 ? tr("RUNNING - %zu of %zu radio open")
                                  : tr("RUNNING - %zu of %zu radios open"),
                      engine_.patchRadios_.size(), radios);
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
    } else {
        cascade::core::formatUtf8(line,
                      radios == 1 ? tr("STOPPED - %zu of %zu radio switched on")
                                  : tr("STOPPED - %zu of %zu radios switched on"),
                      on, radios);
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
    }
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopStyleColor();

    // Below the dome, for whatever comes next on the page.
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + kR * 2.3f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void AppWindow::drawPatchRadioSwitch(pc::Node& n) {
    // LIT while this radio is actually open, amber while it is switched on and
    // waiting (the patch stopped, or the device still opening), dark when off.
    const bool live = engine_.patchRadios_.count(n.id) != 0;
    const ImU32 face = !n.on ? cascade::gui::theme::kEnamel
                       : live ? cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphor, 0.45f)
                              : cascade::gui::theme::withAlpha(cascade::gui::theme::kAmber, 0.35f);
    ImGui::PushStyleColor(ImGuiCol_Button, face);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          cascade::gui::theme::withAlpha(cascade::gui::theme::kIvory, 0.25f));
    if (ImGui::SmallButton(n.on ? trId("ON###radioOn") : trId("OFF###radioOn"))) {
        n.on = !n.on;
        patchUi_.dirty = true;
        cascade::core::diagLogf("patch: radio node %u switched %s", static_cast<unsigned>(n.id),
                                n.on ? "on" : "off");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s",
            n.on ? tr("This radio is switched on. Press to close it alone - the rest\n"
                      "of the patch keeps running.")
                 : tr("This radio is switched off. Press to switch it on; it opens\n"
                      "while the patch is running."));
    }
}

namespace {

}  // namespace

void AppWindow::patchCollectMapTargets(pc::NodeId map) {
    patchMapTracks_.clear();
    patchMapPaths_.clear();
    // Targets are tagged with their module's display name by the plugin UI,
    // which already applies the host's staleness rule - the same list the map
    // pages draw, so a patch map and a map page never disagree about what is
    // there.
    const std::vector<std::string> sources = pc::mapSources(engine_.patchGraph_, map, engine_.patchCatalogue_);
    if (sources.empty()) { return; }
    const auto wanted = [&sources](const std::string& plugin) {
        return std::find(sources.begin(), sources.end(), plugin) != sources.end();
    };
    for (const cascade::core::HostTrack& t : engine_.pluginUi_.tracks()) {
        if (wanted(t.plugin)) { patchMapTracks_.push_back(t); }
    }
    for (const cascade::core::HostPath& p : engine_.pluginUi_.paths()) {
        if (wanted(p.plugin)) { patchMapPaths_.push_back(p); }
    }
}

void AppWindow::drawPatchMapInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    const auto amber = cascade::gui::theme::vec(cascade::gui::theme::kAmber);
    ImGui::Spacing();
    std::size_t wired = 0;
    for (const pc::Wire& w : engine_.patchGraph_.wires()) {
        if (w.to == n.id) { ++wired; }
    }
    ImGui::Text(tr("%zu of %zu inputs wired"), wired, pc::kMapInputs);
    patchCollectMapTargets(n.id);
    for (const std::string& src : pc::mapSources(engine_.patchGraph_, n.id, engine_.patchCatalogue_)) {
        std::size_t count = 0;
        for (const cascade::core::HostTrack& t : patchMapTracks_) {
            if (t.plugin == src) { ++count; }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::Text("%zu", count);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", src.c_str());
    }
    const auto view = patchMapViews_.find(n.id);
    if (view != patchMapViews_.end() && view->second) {
        if (ImGui::Button(trId("Fit to targets"), ImVec2(-FLT_MIN, 0.0f))) {
            view->second->requestFitToTracks();
        }
        if (ImGui::Button(trId("Whole world"), ImVec2(-FLT_MIN, 0.0f))) {
            view->second->requestWholeWorld();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped(
        tr("Wire the map output of up to %zu decoders here - aircraft from one radio, ships "
           "from another - and they share this map. Drag it to pan; zoom with the + and - "
           "keys in its corner, or the mouse wheel."),
        pc::kMapInputs);
    ImGui::PopStyleColor();
}

void AppWindow::drawPatchSquelch(pc::Node& n, float width) {
    bool on = n.squelch;
    if (ImGui::Checkbox(trId("Squelch##sq"), &on)) {
        n.squelch = on;
        patchUi_.dirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s",
            tr("Silences this demodulator's sound while the channel is quieter\n"
               "than the threshold - so a speaker or a recording hears signals,\n"
               "not the noise between them. Decoders are still given every sample."));
    }
    if (n.squelch) {
        float db = n.squelchDb;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderFloat("##sqdb", &db, pc::kSquelchMinDb, pc::kSquelchMaxDb, "%.0f dB")) {
            n.squelchDb = db;
            patchUi_.dirty = true;
        }
    }
    // WHERE TO SET IT: the level the gate is judging, and whether it is open.
    const pc::NodeId chan = demodChannel(engine_.patchGraph_, n.id);
    const auto r = engine_.patchRadios_.find(pc::radioOf(engine_.patchGraph_, chan));
    float level = 0.0f;
    bool open = false;
    if (chan != pc::kNoNode && r != engine_.patchRadios_.end() &&
        r->second->runner().squelchState(chan, level, open)) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::Text("%.0f dB", static_cast<double>(level));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (!n.squelch || open) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
            ImGui::TextUnformatted(n.squelch ? tr("open") : tr("no squelch"));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
            ImGui::TextUnformatted(tr("closed"));
        }
        ImGui::PopStyleColor();
    }
}

void AppWindow::drawPatchRadioInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    const auto amber = cascade::gui::theme::vec(cascade::gui::theme::kAmber);

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Device"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::vector<PatchDeviceChoice> choices = engine_.patchDeviceChoices();
    if (ImGui::BeginCombo("##patchdevice", engine_.patchDeviceLabel(n.device).c_str())) {
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const PatchDeviceChoice& c = choices[i];
            // ONE DEVICE, ONE RADIO: a device another Radio already has is
            // shown, greyed, with who has it - not hidden, so the user can see
            // why it is not offered.
            const pc::Node* holder = nullptr;
            for (const pc::Node& other : engine_.patchGraph_.nodes()) {
                if (other.id != n.id && other.kind == pc::NodeKind::Radio &&
                    pc::sameDevice(other.device, c.key)) {
                    holder = &other;
                }
            }
            ImGui::PushID(static_cast<int>(i));
            std::string text = c.label;
            if (holder != nullptr) {
                std::string buf;
                cascade::core::formatUtf8(buf, tr("  (used by %s)"), holder->name.c_str());
                text += buf;
            }
            // IN USE BY THE RECEIVER IS NOT "NOT AVAILABLE" (2026-09-23): the
            // receiver lends its radio to the patch when the patch starts, so
            // the row is offered, and says what will happen to it.
            else if (engine_.device_ != nullptr &&
                     pc::sameDevice(c.key, pc::makeDeviceKey(engine_.sourceKind_, engine_.deviceArgs_))) {
                text += tr("  (the receiver's - lent to the patch when it starts)");
            }
            ImGui::BeginDisabled(holder != nullptr);
            // Not trId(): text is composed at runtime (a device's own label
            // plus an already-translated suffix), not a fixed English key, and
            // this Selectable's id is already made unique by the PushID(i)
            // above rather than by its label text.
            if (ImGui::Selectable(text.c_str(), c.key == n.device)) {
                n.device = c.key;
                patchUi_.dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // LOOK AGAIN from here, rather than sending the user to the Source panel:
    // a radio plugged in after the page opened, or one a scan beside an open
    // radio had to leave out, is one press away (2026-09-23).
    ImGui::BeginDisabled(engine_.soapyScanPending_);
    if (ImGui::SmallButton(trId("Look for radios"))) {
        engine_.submitCommand(cascade::core::cmd::make(FOXAPI_OP_SCAN_DEVICES));
    }
    ImGui::EndDisabled();
    if (engine_.soapyScanPending_) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted(tr("looking..."));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Centre (MHz)"));
    double mhz = n.freqHz / 1e6;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputDouble("##patchcentre", &mhz, 0.1, 1.0, "%.6f",
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        setPatchRadioCentre(n, mhz * 1e6);
    }
    drawPatchCentreNote(n);

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Sample rate"));
    char rateText[32];
    std::snprintf(rateText, sizeof(rateText), "%.3f MS/s", radioRate(n) / 1e6);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##patchrate", rateText)) {
        for (const double r : kPatchRatesHz) {
            char t[32];
            std::snprintf(t, sizeof(t), "%.3f MS/s", r / 1e6);
            if (ImGui::Selectable(t, std::fabs(r - radioRate(n)) < 1.0)) {
                n.rateHz = r;
                patchUi_.dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    // What it is actually doing.
    ImGui::Spacing();
    const auto run = engine_.patchRadios_.find(n.id);
    if (run != engine_.patchRadios_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::Text(tr("running %.3f MS/s"), run->second->rateHz() / 1e6);
        ImGui::Text(tr("at %.6f MHz"), run->second->centreHz() / 1e6);
        ImGui::PopStyleColor();
    } else if (engine_.patchRadioPending_.count(n.id) != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted(tr("opening the device..."));
        ImGui::PopStyleColor();
    }
    const auto err = engine_.patchRadioError_.find(n.id);
    if (err != engine_.patchRadioError_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
        ImGui::TextWrapped("%s", err->second.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped(
        tr("Up to %zu radios, each its own device. While this page is open the receiver runs "
           "on the signal generator; closing it gives the radio back."),
        pc::kMaxRadios);
    ImGui::PopStyleColor();
}

void AppWindow::drawPatchSinkInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Sound goes to"));
    const std::string key = n.device.empty() ? std::string("wav") : n.device;
    std::string current;
    switch (pc::outputKind(key)) {
        case pc::OutputKind::Wav: current = tr("A WAV file"); break;
        case pc::OutputKind::Mp3: current = tr("An MP3 file"); break;
        case pc::OutputKind::Speakers: current = tr("The speakers"); break;
        case pc::OutputKind::Device: current = pc::outputDeviceName(key); break;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##patchoutput", current.c_str())) {
        if (ImGui::Selectable(trId("A WAV file"), pc::outputKind(key) == pc::OutputKind::Wav)) {
            n.device = "wav";
            patchUi_.dirty = true;
        }
        const bool mp3 = cascade::core::Mp3Writer::available();
        ImGui::BeginDisabled(!mp3);
        if (ImGui::Selectable(mp3 ? trId("An MP3 file") : trId("An MP3 file (needs Windows)"),
                              pc::outputKind(key) == pc::OutputKind::Mp3)) {
            n.device = "mp3";
            patchUi_.dirty = true;
        }
        ImGui::EndDisabled();
        if (ImGui::Selectable(trId("The speakers"),
                              pc::outputKind(key) == pc::OutputKind::Speakers)) {
            n.device = "speakers";
            patchUi_.dirty = true;
        }
        for (std::size_t i = 0; i < engine_.devices_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string k = pc::makeOutputDeviceKey(engine_.devices_[i].name);
            if (ImGui::Selectable(engine_.devices_[i].name.c_str(), key == k)) {
                n.device = k;
                patchUi_.dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    for (const auto& d : engine_.patchDests_) {
        if (d.first != n.id || !d.second) { continue; }
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::TextWrapped("%s", d.second->describe().c_str());
        ImGui::Text("%.1f s", static_cast<double>(d.second->samples()) / pc::kOutRateHz);
        ImGui::PopStyleColor();
        const std::string e = d.second->error();
        if (!e.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
            ImGui::TextWrapped("%s", e.c_str());
            ImGui::PopStyleColor();
        }
    }
    const auto err = engine_.patchDestError_.find(n.id);
    if (err != engine_.patchDestError_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
        ImGui::TextWrapped("%s", err->second.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    if (pc::outputKind(key) == pc::OutputKind::Wav || pc::outputKind(key) == pc::OutputKind::Mp3) {
        ImGui::TextWrapped(tr("Files go in %s, one per speaker, named after it."),
                           engine_.recordDir_.c_str());
    }
    ImGui::PopStyleColor();
}

}  // namespace cascade::gui
